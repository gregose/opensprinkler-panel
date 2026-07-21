// M6 — UX & state machine
//
// Single-screen panel firmware for the ESP32-3248S035R (CYD).
// Wires lib/panel_state (pure C++ state machine) to the LVGL UI, TFT_eSPI
// display/touch, and the OpenSprinkler HTTP API via OsClient.
//
// Hardware: ESP32-WROOM-32E, ST7796U display (480×320 landscape), XPT2046
// resistive touch — both on the same SPI bus. See docs/03 for the pin map.
//
// Architecture: single-task Arduino loop (docs/03 §concurrency).
//   - loop() calls lv_timer_handler() every ~5 ms.
//   - A millis()-based scheduler drives the ~2 s /jc poll and the 5-min sleep.
//   - User actions (LVGL event callbacks) call PanelState, which calls OsClient.
//   - PanelState::tick() decides when to poll; the loop issues the actual HTTP
//     call and feeds the result back via on_jc()/on_jc_error().
//
// Provisioning (WiFi + OS host + password) uses Preferences NVS.
// Full WiFiManager captive portal is M4; this file reads existing NVS keys.

#include <Arduino.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <algorithm>
#include <memory>

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <lvgl.h>
#include <TFT_eSPI.h>

#include "os_client.h"
#include "panel_config.h"
#include "panel_state.h"
#include "station_model.h"
#include "ui_font_countdown_48.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int PIN_BACKLIGHT = 27;
static constexpr int PIN_BOOT_BTN  = 0;
static constexpr int LEDC_CHANNEL  = 0;
static constexpr int LEDC_FREQ_HZ  = 5000;
static constexpr int LEDC_RES_BITS = 8;
static constexpr uint8_t BACKLIGHT_ON  = 255;
static constexpr uint8_t BACKLIGHT_OFF = 0;

static constexpr int SCREEN_W = 480;
static constexpr int SCREEN_H = 320;
// Draw buffer: 480×4 pixels — keeps BSS small on the no-PSRAM ESP32.
static constexpr int DRAW_BUF_LINES = 40;
static constexpr unsigned long BOOT_HOLD_EDIT_MS    = 3000;
static constexpr unsigned long BOOT_HOLD_FACTORY_MS = 10000;
static constexpr int           PORTAL_EDIT_TIMEOUT_S = 180;
static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr int           CALIBRATION_COMPLETE_DELAY_MS = 1000;
static constexpr uint32_t UI_TICK_MS = 5;
static constexpr uint32_t NETWORK_LOOP_MS = 50;
static constexpr uint32_t JC_POLL_INTERVAL_MS = 2000;
static constexpr uint32_t JN_RETRY_INITIAL_MS = 1000;
static constexpr uint32_t JN_RETRY_MAX_MS = 10000;
static constexpr int LINK_RETRY_LIMIT = 3;
static constexpr const char* HEARTBEAT_LOG_FORMAT =
    "[HB] ms=%lu ui=%lu net=%lu phase=%s heap=%u ui_hwm=%u net_hwm=%u\n";

// Boot-hold mode: determined at startup by measuring how long BOOT is held.
enum class BootMode { kNormal, kEditConfig, kFactoryClear };

// NVS keys (matches M4 provisioning schema).
static constexpr const char* NVS_NS        = "osp-panel";
static constexpr const char* NVS_SSID      = "wifi_ssid";
static constexpr const char* NVS_PASS      = "wifi_pass";
static constexpr const char* NVS_HOST      = "os_host";
static constexpr const char* NVS_PWMD5     = "os_pw_md5";
static constexpr const char* NVS_OTA       = "ota_pass";
static constexpr const char* NVS_DEV_LOG   = "dev_log";
static constexpr const char* NVS_TOUCHCAL  = "touch_cal";
static constexpr const char* NVS_RT        = "run_time_s";
static constexpr const char* NVS_AA        = "auto_adv";
static constexpr const char* DEFAULT_PW_MD5 = "a6d82bced638de3def1e9bbb4983225c";
static constexpr const char* DEFAULT_OS_HOST = "192.168.1.100";
static constexpr const char* PROVISION_AP_SSID = "OSPanel-Setup";

// Visual tokens (docs/01 §5).
static constexpr uint32_t CLR_BG    = 0x07100f;
static constexpr uint32_t CLR_TEXT  = 0xe9f2ef;
static constexpr uint32_t CLR_MUTED = 0x7f938f;
static constexpr uint32_t CLR_TEAL  = 0x35d0c3;
static constexpr uint32_t CLR_AMBER = 0xf2a63b;
static constexpr uint32_t CLR_RED   = 0xff5b5b;
static constexpr uint32_t CLR_LINE  = 0x1a2e2b;

// Build an lv_color_t from a 0xRRGGBB constant.
static inline lv_color_t hex_color(uint32_t hex) {
    return lv_color_make((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

// Compatibility: hide/show using LVGL 9 flags.
static inline void obj_set_hidden(lv_obj_t* obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Hardware objects
// ---------------------------------------------------------------------------
static TFT_eSPI tft;

// LVGL draw buffer (static, internal SRAM — no PSRAM on this board).
// RGB565 render target = 2 bytes/px. Must be aligned to LVGL 9's
// LV_DRAW_BUF_ALIGN; lv_display_set_buffers() asserts on a misaligned buffer
// (a bare 1-byte-aligned array silently hangs during init).
alignas(64) static uint8_t draw_buf[SCREEN_W * DRAW_BUF_LINES * 2];

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
static osp::StationModel g_model;
static std::unique_ptr<osp::OsClient>   g_client;
static std::unique_ptr<osp::PanelState> g_ps;
static SemaphoreHandle_t g_state_mutex = nullptr;
static volatile uint32_t g_model_version = 0;
static volatile uint32_t g_ui_beat = 0;
static volatile uint32_t g_net_beat = 0;
static volatile uint32_t g_phase_snapshot = static_cast<uint32_t>(osp::Phase::Idle);
static volatile bool g_dev_log_enabled = false;
static bool g_consume_touch_until_release = false;
static TaskHandle_t g_ui_task_handle = nullptr;
static TaskHandle_t g_net_task_handle = nullptr;

// NVS config cache.
static String g_os_host;
static String g_pw_md5;

class StateLock {
public:
    explicit StateLock(TickType_t wait = portMAX_DELAY)
        : locked_(g_state_mutex &&
                  xSemaphoreTake(g_state_mutex, wait) == pdTRUE) {}

    ~StateLock() {
        if (locked_) xSemaphoreGive(g_state_mutex);
    }

    explicit operator bool() const { return locked_; }

private:
    bool locked_ = false;
};

static void cache_phase_snapshot_unlocked() {
    if (g_ps) {
        g_phase_snapshot = static_cast<uint32_t>(g_ps->view().phase);
    }
}

static const char* phase_snapshot_name(uint32_t phase) {
    switch (static_cast<osp::Phase>(phase)) {
        case osp::Phase::Idle:
            return "Idle";
        case osp::Phase::Running:
            return "Running";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// OTA responder + TCP log mirror — always compiled into production firmware.
//
// ArduinoOTA is LAN-only and password-gated: begin() is called only when the
// NVS `ota_pass` key is non-empty, so an unprovisioned device never exposes
// an unauthenticated OTA endpoint.  The TCP log server (port 2323) starts only
// when the NVS `dev_log` bool is true (default false).  When dev_log is false,
// TeeSerial forwards only to UART0 — negligible overhead.
// ---------------------------------------------------------------------------

// Save a pointer to the real UART0 Serial before the #define redirect below,
// so TeeSerial methods and OTA callbacks can reach UART0 without the macro.
static HardwareSerial* const g_hw_serial = &Serial;

static constexpr uint16_t LOG_PORT = 2323;
static WiFiServer g_log_server(LOG_PORT);
static WiFiClient g_log_client;
static bool g_ota_started = false;
static bool g_log_server_started = false;

// TeeSerial: Print subclass that writes to UART0 and, when a TCP log client is
// connected, also to that client.  Keeps the per-character path tiny to avoid
// budget pressure on the no-PSRAM ESP32.
class TeeSerial : public Print {
public:
    void begin(unsigned long baud) { g_hw_serial->begin(baud); }
    int  availableForWrite()       { return g_hw_serial->availableForWrite(); }

    size_t write(uint8_t c) override {
        g_hw_serial->write(c);
        if (g_log_client && g_log_client.connected()) { g_log_client.write(c); }
        return 1;
    }
    size_t write(const uint8_t* buf, size_t n) override {
        g_hw_serial->write(buf, n);
        if (g_log_client && g_log_client.connected()) { g_log_client.write(buf, n); }
        return n;
    }
};
static TeeSerial g_tee_serial;

// Redirect all Serial.xxx calls in this translation unit to g_tee_serial.
// Framework libraries (WiFiManager, HTTP, etc.) are compiled separately and
// continue to write to UART0 directly, so USB still shows everything.
#define Serial g_tee_serial

static void dev_loop_init(const String& ota_pass, bool dev_log) {
    g_dev_log_enabled = dev_log;

    // mDNS — stable hostname so espota.py can find the device without an IP.
    MDNS.begin("ospanel");

    // ArduinoOTA responder: only start when a password is provisioned.
    // Never expose an unauthenticated OTA endpoint on the LAN.
    if (!ota_pass.isEmpty()) {
        ArduinoOTA.setHostname("ospanel");
        ArduinoOTA.setPassword(ota_pass.c_str());
        ArduinoOTA.onStart([]() {
            // Print directly to UART0 — don't try to use the TCP client mid-OTA.
            g_hw_serial->println("[OTA] Start");
        });
        ArduinoOTA.onEnd([]() {
            g_hw_serial->println("[OTA] End — rebooting");
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            g_hw_serial->printf("[OTA] %u%%\r", progress * 100 / total);
        });
        ArduinoOTA.onError([](ota_error_t error) {
            g_hw_serial->printf("[OTA] Error[%u]\n", error);
        });
        ArduinoOTA.begin();
        g_ota_started = true;
        g_hw_serial->println("[OTA] enabled — ospanel.local");
    } else {
        g_hw_serial->println("[OTA] disabled — set ota_pass in config portal to enable");
    }

    // TCP log server — single-client, last-connected-wins.
    // Only started when dev_log NVS flag is true (default false).
    if (dev_log) {
        g_log_server.begin();
        g_log_server_started = true;
        g_hw_serial->printf("[LOG] TCP log server on port %u\n", LOG_PORT);
    } else {
        g_hw_serial->println("[LOG] TCP log disabled (set dev_log in config portal to enable)");
    }
}

static void dev_loop_handle() {
    if (g_ota_started) {
        ArduinoOTA.handle();
    }

    if (g_log_server_started) {
        // Accept new TCP log client (single-slot: drops any stale connection).
        if (g_log_server.hasClient()) {
            if (g_log_client) g_log_client.stop();
            g_log_client = g_log_server.accept();
            g_hw_serial->println("[LOG] client connected");
            g_log_client.println("[LOG] OSPanel log stream");
        }
        // Silently drop dead connections so write() doesn't block.
        if (g_log_client && !g_log_client.connected()) {
            g_log_client.stop();
        }
    }
}

// ---------------------------------------------------------------------------
// Provisioning + NVS helpers
// ---------------------------------------------------------------------------
static String md5_hex(const String& plaintext) {
    MD5Builder md5;
    md5.begin();
    md5.add(plaintext);
    md5.calculate();
    return md5.toString();
}

static void draw_boot_message(const char* line1,
                              const char* line2 = nullptr,
                              const char* line3 = nullptr) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(8, 12);
    tft.println("OpenSprinkler panel");
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(8, 52);
    tft.println(line1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    if (line2) {
        tft.setCursor(8, 84);
        tft.println(line2);
    }
    if (line3) {
        tft.setCursor(8, 108);
        tft.println(line3);
    }
}

static void load_config_from_nvs(String* ssid,
                                 String* pass,
                                 String* os_host,
                                 String* pw_md5,
                                 String* ota_pass,
                                 int* run_time_s,
                                 bool* dev_log = nullptr,
                                 bool* auto_advance = nullptr) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    if (ssid) *ssid = prefs.getString(NVS_SSID, "");
    if (pass) *pass = prefs.getString(NVS_PASS, "");
    if (os_host) *os_host = prefs.getString(NVS_HOST, "");
    if (pw_md5) *pw_md5 = prefs.getString(NVS_PWMD5, "");
    if (ota_pass) *ota_pass = prefs.getString(NVS_OTA, "");
    if (run_time_s) *run_time_s = prefs.getInt(NVS_RT, osp::PanelState::kDefaultRunTime);
    if (dev_log) *dev_log = prefs.getBool(NVS_DEV_LOG, false);
    if (auto_advance) *auto_advance = prefs.getBool(NVS_AA, false);
    prefs.end();
}

static void save_config_to_nvs(const String& ssid,
                               const String& pass,
                               const String& os_host,
                               const String& pw_md5,
                               const String& ota_pass,
                               bool dev_log) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putString(NVS_SSID, ssid);
    prefs.putString(NVS_PASS, pass);
    prefs.putString(NVS_HOST, os_host);
    prefs.putString(NVS_PWMD5, pw_md5);
    if (ota_pass.isEmpty()) prefs.remove(NVS_OTA);
    else                    prefs.putString(NVS_OTA, ota_pass);
    prefs.putBool(NVS_DEV_LOG, dev_log);
    prefs.end();
}

static void save_run_time_to_nvs(int run_time_s) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putInt(NVS_RT, run_time_s);
    prefs.end();
}

static void save_auto_adv_to_nvs(bool on) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putBool(NVS_AA, on);
    prefs.end();
}

// Measure how long the BOOT button is held at startup and return the
// corresponding mode.  On-screen prompts are shown at each threshold.
//   < BOOT_HOLD_EDIT_MS    → kNormal  (button released early or not pressed)
//   < BOOT_HOLD_FACTORY_MS → kEditConfig  (non-destructive config edit)
//   ≥ BOOT_HOLD_FACTORY_MS → kFactoryClear (full factory wipe)
static BootMode measure_boot_hold() {
    if (digitalRead(PIN_BOOT_BTN) != LOW) return BootMode::kNormal;

    draw_boot_message("Hold BOOT to reconfigure",
                      "3 s = edit config",
                      "10 s = factory reset");

    const unsigned long t0 = millis();
    BootMode mode = BootMode::kNormal;

    while (digitalRead(PIN_BOOT_BTN) == LOW) {
        const unsigned long held = millis() - t0;
        if (mode == BootMode::kNormal && held >= BOOT_HOLD_EDIT_MS) {
            mode = BootMode::kEditConfig;
            draw_boot_message("Release to edit config",
                              "Keep holding 10 s to erase all");
        }
        if (mode == BootMode::kEditConfig && held >= BOOT_HOLD_FACTORY_MS) {
            mode = BootMode::kFactoryClear;
            draw_boot_message("Erasing all config...");
            break;
        }
        delay(10);
    }
    return mode;
}

static bool connect_wifi(const String& ssid, const String& pass) {
    if (ssid.isEmpty()) return false;
    Serial.printf("Connecting to %s\n", ssid.c_str());
    draw_boot_message("Connecting Wi-Fi", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("WiFi connect timeout");
    return false;
}

// Shared helper: build NVS-merged OS/OTA/log fields from portal parameter values.
// Saves a complete record to NVS.  WiFi creds (ssid/pass) are passed in as-is
// (AP portal supplies new ones; STA portal keeps the existing ones).
static void save_portal_params_to_nvs(const String& ssid,
                                      const String& pass,
                                      const String& current_host,
                                      const String& current_pw_md5,
                                      const WiFiManagerParameter& os_host_param,
                                      const WiFiManagerParameter& os_pass_param,
                                      const WiFiManagerParameter& ota_pass_param,
                                      const WiFiManagerParameter& dev_log_param) {
    const String normalized_host = osp::normalize_os_host(current_host.c_str()).c_str();

    String saved_host = osp::normalize_os_host(os_host_param.getValue()).c_str();
    if (saved_host.isEmpty()) saved_host = normalized_host;
    if (saved_host.isEmpty()) saved_host = DEFAULT_OS_HOST;

    const String plain_os_pass = osp::trim_ascii(os_pass_param.getValue()).c_str();
    String saved_pw_md5 = osp::normalize_md5_hex(current_pw_md5.c_str()).c_str();
    if (!plain_os_pass.isEmpty()) {
        saved_pw_md5 = osp::normalize_md5_hex(md5_hex(plain_os_pass).c_str()).c_str();
    } else if (!osp::is_valid_md5_hex(saved_pw_md5.c_str())) {
        saved_pw_md5 = DEFAULT_PW_MD5;
    }

    const String saved_ota_pass = osp::trim_ascii(ota_pass_param.getValue()).c_str();
    const bool saved_dev_log = (strcmp(dev_log_param.getValue(), "1") == 0);
    save_config_to_nvs(ssid, pass, saved_host, saved_pw_md5, saved_ota_pass, saved_dev_log);
    Serial.printf("Config saved for host %s (pw_md5 %s, ota_pass %s, dev_log %s)\n",
                  saved_host.c_str(),
                  osp::is_valid_md5_hex(saved_pw_md5.c_str()) ? "set" : "invalid",
                  saved_ota_pass.isEmpty() ? "empty" : "set",
                  saved_dev_log ? "true" : "false");
}

// Start the WiFiManager captive-portal (AP mode).
//
// current_ssid / current_pass are the NVS-stored WiFi credentials.  They are
// used as a fallback if WiFi.SSID()/psk() are empty after the portal connects
// (Bug B: WiFiManager can connect via its own store while leaving WiFi.SSID()
// unset, which would produce an incomplete NVS record and a reprovision loop).
//
// non_destructive: true  → set portal timeout (PORTAL_EDIT_TIMEOUT_S) and add
//                           "Reset touch calibration" checkbox.
//                  false → no timeout, no touch-cal checkbox.
//
// touch_cal_reset_out: if non-null and non_destructive, set to true when the
//                      user checked the reset-touch-cal checkbox on save.
static bool start_provisioning_portal(const String& current_ssid,
                                      const String& current_pass,
                                      const String& current_host,
                                      const String& current_pw_md5,
                                      const String& current_ota_pass,
                                      bool current_dev_log,
                                      bool non_destructive,
                                      bool* touch_cal_reset_out) {
    draw_boot_message(non_destructive ? "Edit config" : "Setup mode",
                      "Join Wi-Fi OSPanel-Setup",
                      "Open the captive portal");

    char host_buf[65] = {};
    char ota_buf[65] = {};
    String normalized_host = osp::normalize_os_host(current_host.c_str()).c_str();
    String trimmed_ota = osp::trim_ascii(current_ota_pass.c_str()).c_str();
    normalized_host.toCharArray(host_buf, sizeof(host_buf));
    trimmed_ota.toCharArray(ota_buf, sizeof(ota_buf));

    WiFiManager wm;

    if (non_destructive) {
        wm.setConfigPortalTimeout(PORTAL_EDIT_TIMEOUT_S);
    }

    WiFiManagerParameter os_host_param("os_host", "OpenSprinkler host",
                                       host_buf, sizeof(host_buf));
    WiFiManagerParameter os_pass_param("os_pass", "OpenSprinkler device password",
                                       "", 65,
                                       "type='password' autocomplete='off'");
    WiFiManagerParameter ota_pass_param("ota_pass", "OTA password (leave blank to disable OTA)",
                                        ota_buf, sizeof(ota_buf),
                                        "type='password' autocomplete='off'");
    WiFiManagerParameter dev_log_param("dev_log",
                                       "Enable remote debug log (port 2323)",
                                       "1", 2,
                                       current_dev_log ? "type='checkbox' checked"
                                                       : "type='checkbox'",
                                       WFM_LABEL_AFTER);
    WiFiManagerParameter reset_touch_param("reset_touch",
                                           "Reset touch calibration",
                                           "1", 2,
                                           "type='checkbox'",
                                           WFM_LABEL_AFTER);

    wm.setAPCallback([](WiFiManager* portal) {
        draw_boot_message("Setup mode", portal->getConfigPortalSSID().c_str(),
                          "Open the captive portal");
    });
    wm.addParameter(&os_host_param);
    wm.addParameter(&os_pass_param);
    wm.addParameter(&ota_pass_param);
    wm.addParameter(&dev_log_param);
    if (non_destructive) {
        wm.addParameter(&reset_touch_param);
    }

    if (!wm.startConfigPortal(PROVISION_AP_SSID)) {
        Serial.println("Provisioning portal exited without WiFi connection");
        return false;
    }

    // Bug B fix: WiFiManager may connect via its own stored creds and leave
    // WiFi.SSID()/psk() empty.  Fall back to the NVS-stored values so the
    // saved record is always complete and has_provisioning_config() stays true.
    const String saved_ssid = !WiFi.SSID().isEmpty() ? WiFi.SSID() : current_ssid;
    const String saved_pass = !WiFi.psk().isEmpty()  ? WiFi.psk()  : current_pass;

    save_portal_params_to_nvs(saved_ssid, saved_pass, current_host, current_pw_md5,
                               os_host_param, os_pass_param, ota_pass_param, dev_log_param);

    if (non_destructive && touch_cal_reset_out) {
        *touch_cal_reset_out = (strcmp(reset_touch_param.getValue(), "1") == 0);
    }
    return true;
}

// Serve the edit-config portal over the existing STA connection (non-blocking).
// The device stays on home WiFi; the user browses to the LAN IP.  Runs a
// wm.process() loop for up to PORTAL_EDIT_TIMEOUT_S seconds.
//
// Returns true if the user saved params (NVS record written).
// Returns false if the loop timed out without a save.
//
// touch_cal_reset_out: set to true if the user checked "Reset touch calibration".
static bool start_sta_web_portal(const String& current_ssid,
                                 const String& current_pass,
                                 const String& current_host,
                                 const String& current_pw_md5,
                                 const String& current_ota_pass,
                                 bool current_dev_log,
                                 bool* touch_cal_reset_out) {
    char host_buf[65] = {};
    char ota_buf[65] = {};
    String normalized_host = osp::normalize_os_host(current_host.c_str()).c_str();
    String trimmed_ota = osp::trim_ascii(current_ota_pass.c_str()).c_str();
    normalized_host.toCharArray(host_buf, sizeof(host_buf));
    trimmed_ota.toCharArray(ota_buf, sizeof(ota_buf));

    WiFiManager wm;

    WiFiManagerParameter os_host_param("os_host", "OpenSprinkler host",
                                       host_buf, sizeof(host_buf));
    WiFiManagerParameter os_pass_param("os_pass", "OpenSprinkler device password",
                                       "", 65,
                                       "type='password' autocomplete='off'");
    WiFiManagerParameter ota_pass_param("ota_pass", "OTA password (leave blank to disable OTA)",
                                        ota_buf, sizeof(ota_buf),
                                        "type='password' autocomplete='off'");
    WiFiManagerParameter dev_log_param("dev_log",
                                       "Enable remote debug log (port 2323)",
                                       "1", 2,
                                       current_dev_log ? "type='checkbox' checked"
                                                       : "type='checkbox'",
                                       WFM_LABEL_AFTER);
    WiFiManagerParameter reset_touch_param("reset_touch",
                                                   "Reset touch calibration",
                                                   "1", 2,
                                                   "type='checkbox'",
                                                   WFM_LABEL_AFTER);

    wm.addParameter(&os_host_param);
    wm.addParameter(&os_pass_param);
    wm.addParameter(&ota_pass_param);
    wm.addParameter(&dev_log_param);
    wm.addParameter(&reset_touch_param);

    bool params_saved = false;
    wm.setSaveParamsCallback([&params_saved]() { params_saved = true; });

    wm.startWebPortal();

    String ip_url = "http://";
    ip_url += WiFi.localIP().toString();
    ip_url += "/";
    Serial.printf("STA web portal started at %s\n", ip_url.c_str());
    draw_boot_message("Edit config at:", ip_url.c_str(),
                      "Save to apply, wait to skip");

    const unsigned long t0 = millis();
    const unsigned long timeout_ms = (unsigned long)PORTAL_EDIT_TIMEOUT_S * 1000UL;
    while ((millis() - t0) < timeout_ms) {
        wm.process();
        if (params_saved) break;
        delay(10);
    }
    wm.stopWebPortal();

    if (!params_saved) {
        Serial.println("STA edit portal timed out without save");
        return false;
    }

    // WiFi creds unchanged — keep the existing NVS ssid/pass.
    save_portal_params_to_nvs(current_ssid, current_pass, current_host, current_pw_md5,
                               os_host_param, os_pass_param, ota_pass_param, dev_log_param);

    if (touch_cal_reset_out) {
        *touch_cal_reset_out = (strcmp(reset_touch_param.getValue(), "1") == 0);
    }
    return true;
}

static bool ensure_network_config() {
    String ssid;
    String pass;
    String ota_pass;
    bool dev_log = false;
    int ignored_run_time = osp::PanelState::kDefaultRunTime;
    load_config_from_nvs(&ssid, &pass, &g_os_host, &g_pw_md5, &ota_pass, &ignored_run_time, &dev_log);

    g_os_host = osp::normalize_os_host(g_os_host.c_str()).c_str();
    g_pw_md5 = osp::normalize_md5_hex(g_pw_md5.c_str()).c_str();

    const BootMode mode = measure_boot_hold();

    // --- Factory clear (≥ 10 s hold) ------------------------------------
    // Wipe the entire app NVS namespace (including touch_cal), reset
    // WiFiManager's own WiFi store (Bug A fix), then open a blank portal.
    if (mode == BootMode::kFactoryClear) {
        Serial.println("BOOT held 10 s: factory clear");
        {
            WiFiManager wm;
            wm.resetSettings();  // Bug A fix: clear WiFiManager's own WiFi store
        }
        WiFi.disconnect(true, true);
        {
            Preferences prefs;
            prefs.begin(NVS_NS, false);
            prefs.clear();  // Wipes everything, including touch_cal
            prefs.end();
        }
        g_os_host = "";
        g_pw_md5 = "";
        ssid = "";
        pass = "";
        ota_pass = "";
        dev_log = false;
        Serial.println("Factory clear complete; starting provisioning portal");
        start_provisioning_portal(ssid, pass, g_os_host, g_pw_md5, ota_pass,
                                   dev_log, false, nullptr);
        load_config_from_nvs(&ssid, &pass, &g_os_host, &g_pw_md5, nullptr, nullptr);
        g_os_host = osp::normalize_os_host(g_os_host.c_str()).c_str();
        g_pw_md5 = osp::normalize_md5_hex(g_pw_md5.c_str()).c_str();
        return WiFi.status() == WL_CONNECTED;
    }

    // --- Non-destructive config edit (3–10 s hold) ----------------------
    // Try to reconnect in STA mode and serve config over the existing WiFi
    // so the user can browse to the device IP on their LAN.  If STA connect
    // fails (creds changed / AP gone), fall back to the AP captive portal.
    // On save or timeout in STA mode: reboot into the UX.
    // On timeout via the AP portal fallback: continue with existing config.
    if (mode == BootMode::kEditConfig) {
        Serial.println("BOOT held 3 s: non-destructive config edit");
        bool touch_cal_reset = false;

        const bool sta_ok = connect_wifi(ssid, pass);
        if (sta_ok) {
            // STA connected — serve config over existing WiFi, no AP needed.
            const bool saved = start_sta_web_portal(ssid, pass, g_os_host,
                                                     g_pw_md5, ota_pass,
                                                     dev_log, &touch_cal_reset);
            if (saved && touch_cal_reset) {
                Serial.println("Touch cal reset: removing NVS key");
                Preferences prefs;
                prefs.begin(NVS_NS, false);
                prefs.remove(NVS_TOUCHCAL);
                prefs.end();
            }
            // Reboot whether or not the user saved — closes the web server
            // cleanly and ensures any config changes take effect.
            Serial.println(saved ? "Config saved; rebooting into UX"
                                 : "STA edit portal timed out; rebooting");
            ESP.restart();
        }

        // STA connect failed — fall back to AP captive portal so the user
        // can still fix the WiFi credentials.
        Serial.println("STA connect failed; falling back to AP captive portal");
        const bool saved = start_provisioning_portal(ssid, pass, g_os_host,
                                                      g_pw_md5, ota_pass,
                                                      dev_log, true, &touch_cal_reset);
        if (saved) {
            if (touch_cal_reset) {
                Serial.println("Touch cal reset: removing NVS key");
                Preferences prefs;
                prefs.begin(NVS_NS, false);
                prefs.remove(NVS_TOUCHCAL);
                prefs.end();
            }
            Serial.println("Config saved; rebooting into UX");
            ESP.restart();
        }
        // AP portal timed out without save — continue with existing config.
        Serial.println("Edit portal timed out; resuming with existing config");
        return connect_wifi(ssid, pass);
    }

    // --- Normal boot ----------------------------------------------------
    bool connected = false;
    const bool has_config = osp::has_provisioning_config(ssid.c_str(),
                                                          g_os_host.c_str(),
                                                          g_pw_md5.c_str());
    if (has_config) {
        connected = connect_wifi(ssid, pass);
    }
    if (!has_config || !connected) {
        Serial.println("Starting provisioning portal");
        start_provisioning_portal(ssid, pass, g_os_host, g_pw_md5, ota_pass,
                                   dev_log, false, nullptr);
        load_config_from_nvs(&ssid, &pass, &g_os_host, &g_pw_md5, nullptr, nullptr);
        g_os_host = osp::normalize_os_host(g_os_host.c_str()).c_str();
        g_pw_md5 = osp::normalize_md5_hex(g_pw_md5.c_str()).c_str();
        connected = (WiFi.status() == WL_CONNECTED) ||
                    connect_wifi(ssid, pass);
    }
    return connected;
}

// ---------------------------------------------------------------------------
// Touch calibration
// ---------------------------------------------------------------------------
// Load a saved calData[5] blob from NVS and apply it via setTouch().
// Returns true if a valid 10-byte calibration was found and applied.
// NVS namespace/key matches the diag firmware so a diag-seeded calibration
// is honored by production and vice-versa.
static bool load_touch_cal() {
    uint16_t calData[5] = {0};
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    const size_t got = prefs.getBytes(NVS_TOUCHCAL, calData, sizeof(calData));
    prefs.end();
    if (got == sizeof(calData)) {
        tft.setTouch(calData);
        Serial.println("Touch: loaded calibration from NVS.");
        return true;
    }
    Serial.println("Touch: no saved calibration.");
    return false;
}

// Run TFT_eSPI's interactive calibration, persist the result, and apply it.
// Must be called with the display active and BEFORE lv_init() — calibrateTouch()
// draws directly via TFT_eSPI and must not fight an active LVGL display.
static void run_touch_calibration() {
    uint16_t calData[5] = {0};
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.println("Touch calibration");
    tft.setTextSize(1);
    tft.println("");
    tft.println(" Tap each highlighted corner arrow.");
    Serial.println("Touch: tap the corner arrows as they appear...");
    tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 20);
    tft.setTouch(calData);
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putBytes(NVS_TOUCHCAL, calData, sizeof(calData));
    prefs.end();
    Serial.println("Touch: calibration complete and saved to NVS.");
    draw_boot_message("Calibration saved");
    delay(CALIBRATION_COMPLETE_DELAY_MS);
}

// Ensure touch is calibrated: load from NVS if present, otherwise run the
// interactive calibration routine and persist the result.
static void ensure_touch_calibration() {
    if (!load_touch_cal()) {
        run_touch_calibration();
    }
}

// ---------------------------------------------------------------------------
// LVGL callbacks
// ---------------------------------------------------------------------------
static void disp_flush_cb(lv_display_t* disp, const lv_area_t* area,
                           uint8_t* px_map) {
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    // px_map is packed RGB565 (LV_COLOR_FORMAT_RGB565, 2 bytes/pixel).
    tft.pushPixels(reinterpret_cast<const uint16_t*>(px_map), w * h);
    tft.endWrite();
    lv_display_flush_ready(disp);
}

static void touchpad_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    uint16_t tx = 0, ty = 0;
    const bool pressed = tft.getTouch(&tx, &ty);

    if (!pressed) {
        g_consume_touch_until_release = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    bool consume = g_consume_touch_until_release;
    {
        StateLock lock;
        if (lock && g_ps) {
            if (g_ps->view().sleeping) {
                g_ps->on_touch(millis());
                consume = true;
                g_consume_touch_until_release = true;
            }
        }
    }

    data->point.x = static_cast<int32_t>(tx);
    data->point.y = static_cast<int32_t>(ty);
    data->state = consume ? LV_INDEV_STATE_RELEASED
                          : LV_INDEV_STATE_PRESSED;
}

// ---------------------------------------------------------------------------
// UI widgets
// ---------------------------------------------------------------------------
// Top bar
static lv_obj_t* lbl_host   = nullptr;

// Signal-meter widget: 4 ascending bar rectangles (Part 3).
struct SigMeter {
    lv_obj_t* bars[4] = {};
};
static SigMeter sig_panel;
static SigMeter sig_ctrl;

// Left panel states
static lv_obj_t* pnl_idle       = nullptr;
static lv_obj_t* lbl_idle_head  = nullptr;
static lv_obj_t* lbl_idle_sub   = nullptr;
static lv_obj_t* pnl_running    = nullptr;
static lv_obj_t* lbl_eyebrow    = nullptr;
static lv_obj_t* lbl_stn_name   = nullptr;
static lv_obj_t* lbl_countdown  = nullptr;

// Bottom action row (running only)
static lv_obj_t* btn_advance    = nullptr;
static lv_obj_t* btn_stop       = nullptr;

// Right panel
static lv_obj_t* btn_rt_minus   = nullptr;
static lv_obj_t* lbl_rt_value   = nullptr;
static lv_obj_t* btn_rt_plus    = nullptr;
static lv_obj_t* sw_auto_adv    = nullptr;

// Grid
static lv_obj_t* lbl_grid_title  = nullptr;
static lv_obj_t* grid_cont       = nullptr;
static lv_obj_t* stn_pills[24]   = {};
static lv_obj_t* stn_pill_lbls[24] = {};  // labels inside pills for recoloring
static int       g_pill_count    = 0;

// Overlays
static lv_obj_t* sleep_overlay  = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void fmt_countdown(char* buf, int secs) {
    if (secs < 0) secs = 0;
    snprintf(buf, 16, "%d:%02d", secs / 60, secs % 60);
}

// Store/retrieve a station sid (int) as lv_obj user_data (void*).
static void set_pill_sid(lv_obj_t* obj, int sid) {
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(static_cast<intptr_t>(sid)));
}
static int get_pill_sid(lv_obj_t* obj) {
    return static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
}

// Create a button with a centered text label.
static lv_obj_t* make_btn(lv_obj_t* parent, const char* text,
                           uint32_t bg, uint32_t fg,
                           const lv_font_t* font = &lv_font_montserrat_14,
                           int radius = 6) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, hex_color(bg), 0);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, hex_color(fg), 0);
    lv_obj_center(lbl);
    return btn;
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------
static void ev_advance(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->advance();
}
static void ev_stop(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->stop();
}
static void ev_rt_minus(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        StateLock lock;
        if (!(lock && g_ps)) return;
        g_ps->set_run_time(g_ps->view().run_time_s - osp::PanelState::kRunTimeStep);
        save_run_time_to_nvs(g_ps->view().run_time_s);
    }
}
static void ev_rt_plus(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        StateLock lock;
        if (!(lock && g_ps)) return;
        g_ps->set_run_time(g_ps->view().run_time_s + osp::PanelState::kRunTimeStep);
        save_run_time_to_nvs(g_ps->view().run_time_s);
    }
}
static void ev_auto_adv(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        StateLock lock;
        if (!(lock && g_ps)) return;
        g_ps->set_auto_advance(!g_ps->view().auto_advance);
        save_auto_adv_to_nvs(g_ps->view().auto_advance);
    }
}
static void ev_pill(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        StateLock lock;
        if (!(lock && g_ps)) return;
        lv_obj_t* pill = static_cast<lv_obj_t*>(lv_event_get_target(e));
        g_ps->select_station(get_pill_sid(pill));
    }
}
static void ev_touch_any(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        StateLock lock;
        if (lock && g_ps) g_ps->on_touch(millis());
    }
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

// Layout constants (all in pixels, 480×320 landscape).
static constexpr int TOP_H    = 26;
static constexpr int GRID_H   = 112;  // 108→112: container=92px fits 2×pill_h(40)+gap(6)+pad(4)=90px
static constexpr int ACTION_H = 52;
static constexpr int RIGHT_W  = 190;
static constexpr int LEFT_W   = SCREEN_W - RIGHT_W;  // 290 px
// Content area between top bar and grid.
static constexpr int CONTENT_Y = TOP_H + 1;
static constexpr int CONTENT_H = SCREEN_H - CONTENT_Y - GRID_H;
// Panels stop ACTION_H above the grid.
static constexpr int PANEL_H   = CONTENT_H - ACTION_H;
static constexpr int ACTION_Y  = CONTENT_Y + PANEL_H;
static constexpr int GRID_Y    = ACTION_Y + ACTION_H;

// ---------------------------------------------------------------------------
// Signal-meter helpers (Part 3)
// ---------------------------------------------------------------------------

// Build a compact drawn RSSI meter into `parent` (a pre-created flex-row group).
// Layout: "PANEL"/"CTRL" label followed by 4 ascending bar rectangles.
static SigMeter build_sig_meter(lv_obj_t* parent, const char* txt) {
    SigMeter m;

    // Flex-row outer container — transparent, no border, no padding.
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_size(row, LV_SIZE_CONTENT, TOP_H);
    lv_obj_set_style_pad_column(row, 3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // "PANEL" / "CTRL" text label.
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, hex_color(CLR_MUTED), 0);

    // 22×10 sub-container for the 4 ascending bar rects (absolute layout).
    lv_obj_t* bc = lv_obj_create(row);
    lv_obj_remove_style_all(bc);
    lv_obj_set_style_bg_opa(bc, LV_OPA_TRANSP, 0);
    lv_obj_set_size(bc, 22, 10);   // width: 4 bars×4 px + 3 gaps×2 px = 22; height: tallest bar = 10
    lv_obj_clear_flag(bc, LV_OBJ_FLAG_SCROLLABLE);

    static const int bh[4] = {4, 6, 8, 10};
    for (int i = 0; i < 4; ++i) {
        m.bars[i] = lv_obj_create(bc);
        lv_obj_set_size(m.bars[i], 4, bh[i]);
        lv_obj_set_style_border_width(m.bars[i], 0, 0);
        lv_obj_set_style_radius(m.bars[i], 1, 0);
        lv_obj_set_pos(m.bars[i], i * 6, 10 - bh[i]);  // bottom-align bars
        lv_obj_set_style_bg_color(m.bars[i], hex_color(CLR_LINE), 0);
        lv_obj_set_style_bg_opa(m.bars[i], LV_OPA_COVER, 0);
    }

    return m;
}

// Update bar colours: first display_bars(quality, connected) bars filled,
// remainder dim. Colour is by quality tier so the two 1-bar connected cases
// (weak amber vs very-weak red) are distinguishable.
static void update_sig_meter(const SigMeter& m, int quality, bool connected) {
    const int n = osp::display_bars(quality, connected);
    const uint32_t fill_clr = (quality >= 3) ? CLR_TEAL
                            : (quality >= 1)  ? CLR_AMBER
                                              : CLR_RED;
    for (int i = 0; i < 4; ++i) {
        lv_obj_set_style_bg_color(m.bars[i],
            hex_color(i < n ? fill_clr : CLR_LINE), 0);
    }
}

static void build_ui() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, hex_color(CLR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, ev_touch_any, LV_EVENT_PRESSED, nullptr);

    // ---- Top bar -------------------------------------------------------
    {
        lv_obj_t* bar = lv_obj_create(scr);
        lv_obj_set_size(bar, SCREEN_W, TOP_H);
        lv_obj_set_pos(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 2, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        lbl_host = lv_label_create(bar);
        lv_obj_set_style_text_font(lbl_host, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl_host, LV_ALIGN_LEFT_MID, 4, 0);
        lv_label_set_text(lbl_host, LV_SYMBOL_WIFI " Connected");

        // Drawn signal meters in a right-aligned flex-row group (PANEL left, CTRL right).
        lv_obj_t* sig_group = lv_obj_create(bar);
        lv_obj_remove_style_all(sig_group);
        lv_obj_set_style_bg_opa(sig_group, LV_OPA_TRANSP, 0);
        lv_obj_set_size(sig_group, LV_SIZE_CONTENT, TOP_H);
        lv_obj_set_style_pad_column(sig_group, 16, 0);
        lv_obj_clear_flag(sig_group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(sig_group, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(sig_group, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sig_group, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(sig_group, LV_ALIGN_RIGHT_MID, -4, 0);

        sig_panel = build_sig_meter(sig_group, "PANEL");
        sig_ctrl  = build_sig_meter(sig_group, "CTRL");
    }

    // ---- Left panel: Idle ---------------------------------------------
    pnl_idle = lv_obj_create(scr);
    lv_obj_set_size(pnl_idle, LEFT_W, PANEL_H);
    lv_obj_set_pos(pnl_idle, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(pnl_idle, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(pnl_idle, 0, 0);
    lv_obj_set_style_pad_all(pnl_idle, 14, 0);
    lv_obj_clear_flag(pnl_idle, LV_OBJ_FLAG_SCROLLABLE);

    lbl_idle_head = lv_label_create(pnl_idle);
    lv_label_set_text(lbl_idle_head, "Select a station");
    lv_obj_set_style_text_font(lbl_idle_head, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_idle_head, hex_color(CLR_TEXT), 0);
    lv_obj_align(lbl_idle_head, LV_ALIGN_TOP_LEFT, 0, 14);

    lbl_idle_sub = lv_label_create(pnl_idle);
    lv_label_set_text(lbl_idle_sub, "Tap a station below to start");
    lv_obj_set_style_text_font(lbl_idle_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_idle_sub, hex_color(CLR_TEAL), 0);
    lv_label_set_long_mode(lbl_idle_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_idle_sub, LEFT_W - 28);  // honour pad_all=14 each side
    lv_obj_align(lbl_idle_sub, LV_ALIGN_TOP_LEFT, 0, 50);

    // ---- Left panel: Running ------------------------------------------
    pnl_running = lv_obj_create(scr);
    lv_obj_set_size(pnl_running, LEFT_W, PANEL_H);
    lv_obj_set_pos(pnl_running, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(pnl_running, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(pnl_running, 0, 0);
    lv_obj_set_style_pad_all(pnl_running, 14, 0);
    lv_obj_clear_flag(pnl_running, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pnl_running, LV_OBJ_FLAG_HIDDEN);  // hidden until running

    lbl_eyebrow = lv_label_create(pnl_running);
    lv_label_set_text(lbl_eyebrow, "STATION 1");
    lv_obj_set_style_text_font(lbl_eyebrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_eyebrow, hex_color(CLR_TEAL), 0);
    lv_obj_align(lbl_eyebrow, LV_ALIGN_TOP_LEFT, 0, 2);

    lbl_stn_name = lv_label_create(pnl_running);
    lv_label_set_text(lbl_stn_name, "");
    lv_obj_set_width(lbl_stn_name, LEFT_W - 28);
    lv_label_set_long_mode(lbl_stn_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_stn_name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_stn_name, hex_color(CLR_TEXT), 0);
    lv_obj_align(lbl_stn_name, LV_ALIGN_TOP_LEFT, 0, 22);

    lbl_countdown = lv_label_create(pnl_running);
    lv_label_set_text(lbl_countdown, "0:00");
    lv_obj_set_style_text_font(lbl_countdown, &ui_font_countdown_48, 0);
    lv_obj_set_style_text_color(lbl_countdown, hex_color(CLR_AMBER), 0);
    // Sit lower in the panel (near the action buttons) so it's clearly
    // separated from the station name above. Font line height is 32 px and the
    // panel content area is ~101 px, so y=66 leaves a comfortable gap under the
    // name without clipping the bottom (66 + 32 = 98 <= 101).
    lv_obj_align(lbl_countdown, LV_ALIGN_TOP_LEFT, 0, 66);

    // ---- Action row (Advance / Stop) -----------------------------------
    static constexpr int ACTION_SIDE_PAD = 10;
    static constexpr int ACTION_GAP = 10;
    const int action_btn_w = (LEFT_W - (2 * ACTION_SIDE_PAD) - ACTION_GAP) / 2;

    btn_advance = make_btn(scr, "Advance " LV_SYMBOL_RIGHT,
                           CLR_TEAL, CLR_BG, &lv_font_montserrat_20, 11);
    lv_obj_set_size(btn_advance, action_btn_w, ACTION_H);
    lv_obj_set_pos(btn_advance, ACTION_SIDE_PAD, ACTION_Y);
    lv_obj_add_flag(btn_advance, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_advance, ev_advance, LV_EVENT_CLICKED, nullptr);

    btn_stop = make_btn(scr, LV_SYMBOL_STOP " Stop",
                        CLR_RED, CLR_BG, &lv_font_montserrat_20, 11);
    lv_obj_set_size(btn_stop, action_btn_w, ACTION_H);
    lv_obj_set_pos(btn_stop, ACTION_SIDE_PAD + action_btn_w + ACTION_GAP, ACTION_Y);
    lv_obj_add_flag(btn_stop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_stop, ev_stop, LV_EVENT_CLICKED, nullptr);

    // ---- Right panel ---------------------------------------------------
    {
        lv_obj_t* pnl = lv_obj_create(scr);
        lv_obj_set_size(pnl, RIGHT_W, CONTENT_H);
        lv_obj_set_pos(pnl, LEFT_W, CONTENT_Y);
        lv_obj_set_style_bg_color(pnl, hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(pnl, 0, 0);
        lv_obj_set_style_pad_all(pnl, 10, 0);
        lv_obj_clear_flag(pnl, LV_OBJ_FLAG_SCROLLABLE);

        static constexpr int STEP_Y = 18;
        static constexpr int STEP_H = 44;
        static constexpr int PANEL_PAD = 10;
        static constexpr int PANEL_CONTENT_W = RIGHT_W - (2 * PANEL_PAD);

        // Run time label
        lv_obj_t* rt_lbl = lv_label_create(pnl);
        lv_label_set_text(rt_lbl, "RUN TIME");
        lv_obj_set_style_text_font(rt_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rt_lbl, hex_color(CLR_MUTED), 0);
        lv_obj_align(rt_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

        // Run-time stepper: [-] MM:SS [+]
        btn_rt_minus = make_btn(pnl, LV_SYMBOL_MINUS,
                                CLR_LINE, CLR_TEXT, &lv_font_montserrat_24, 9);
        lv_obj_set_size(btn_rt_minus, 46, STEP_H);
        lv_obj_align(btn_rt_minus, LV_ALIGN_TOP_LEFT, 0, STEP_Y);
        lv_obj_add_event_cb(btn_rt_minus, ev_rt_minus, LV_EVENT_CLICKED, nullptr);

        lbl_rt_value = lv_label_create(pnl);
        lv_label_set_text(lbl_rt_value, "1:00");
        lv_obj_set_style_text_font(lbl_rt_value, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl_rt_value, hex_color(CLR_TEXT), 0);
        lv_obj_align(lbl_rt_value, LV_ALIGN_TOP_MID, 0,
                     STEP_Y + (STEP_H - lv_font_get_line_height(&lv_font_montserrat_20)) / 2);

        btn_rt_plus = make_btn(pnl, LV_SYMBOL_PLUS,
                               CLR_LINE, CLR_TEXT, &lv_font_montserrat_24, 9);
        lv_obj_set_size(btn_rt_plus, 46, STEP_H);
        lv_obj_align(btn_rt_plus, LV_ALIGN_TOP_RIGHT, 0, STEP_Y);
        lv_obj_add_event_cb(btn_rt_plus, ev_rt_plus, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* divider = lv_obj_create(pnl);
        lv_obj_remove_style_all(divider);
        lv_obj_set_size(divider, PANEL_CONTENT_W, 1);
        lv_obj_set_style_bg_color(divider, hex_color(CLR_LINE), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 0, 80);

        lv_obj_t* row_auto_adv = lv_obj_create(pnl);
        lv_obj_set_size(row_auto_adv, PANEL_CONTENT_W, 40);
        lv_obj_align(row_auto_adv, LV_ALIGN_TOP_LEFT, 0, 94);
        lv_obj_set_style_bg_opa(row_auto_adv, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_auto_adv, 0, 0);
        lv_obj_set_style_pad_all(row_auto_adv, 0, 0);
        lv_obj_add_flag(row_auto_adv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row_auto_adv, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row_auto_adv, ev_auto_adv, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl_aa_title = lv_label_create(row_auto_adv);
        lv_label_set_text(lbl_aa_title, "Auto-advance");
        lv_obj_set_style_text_font(lbl_aa_title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_aa_title, hex_color(CLR_TEXT), 0);
        lv_obj_align(lbl_aa_title, LV_ALIGN_LEFT_MID, 0, 0);

        sw_auto_adv = lv_switch_create(row_auto_adv);
        lv_obj_align(sw_auto_adv, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw_auto_adv, hex_color(CLR_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw_auto_adv, hex_color(CLR_TEAL),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_clear_flag(sw_auto_adv, LV_OBJ_FLAG_CLICKABLE);
    }

    // ---- Grid area -----------------------------------------------------
    lbl_grid_title = lv_label_create(scr);
    lv_label_set_text(lbl_grid_title, "STATIONS");
    lv_obj_set_style_text_font(lbl_grid_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_grid_title, hex_color(CLR_MUTED), 0);
    lv_obj_set_pos(lbl_grid_title, 8, GRID_Y + 2);

    grid_cont = lv_obj_create(scr);
    lv_obj_set_size(grid_cont, SCREEN_W - 8, GRID_H - 20);
    lv_obj_set_pos(grid_cont, 4, GRID_Y + 18);
    lv_obj_set_style_bg_color(grid_cont, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(grid_cont, 0, 0);
    lv_obj_set_style_pad_all(grid_cont, 2, 0);
    lv_obj_set_style_pad_gap(grid_cont, 6, 0);
    lv_obj_clear_flag(grid_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(grid_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid_cont,
                           LV_FLEX_ALIGN_START,
                           LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    // ---- Sleep overlay -------------------------------------------------
    sleep_overlay = lv_obj_create(scr);
    lv_obj_set_size(sleep_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(sleep_overlay, 0, 0);
    lv_obj_set_style_bg_color(sleep_overlay, hex_color(0x000000), 0);
    lv_obj_set_style_bg_opa(sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_EVENT_BUBBLE);
}

// ---------------------------------------------------------------------------
// Build station pill buttons from g_model
// ---------------------------------------------------------------------------
static void build_grid() {
    for (int i = 0; i < g_pill_count; ++i) {
        if (stn_pills[i]) { lv_obj_delete(stn_pills[i]); stn_pills[i] = nullptr; }
        stn_pill_lbls[i] = nullptr;
    }
    g_pill_count = 0;

    const auto& runnable = g_model.runnable_sids();
    const int n = static_cast<int>(runnable.size());
    if (n == 0) return;

    const osp::GridLayout layout = g_model.layout();
    // Pill size: fit layout.cols pills in (SCREEN_W-12) with 6 px gaps.
    const int inner_w = SCREEN_W - 12;
    const int pill_w  = (inner_w - (layout.cols - 1) * 6) / layout.cols;
    const int pill_h  = 40;

    for (int i = 0; i < n && i < 24; ++i) {
        const int sid = runnable[i];
        lv_obj_t* pill = lv_btn_create(grid_cont);
        lv_obj_set_size(pill, pill_w, pill_h);
        lv_obj_set_style_bg_color(pill, hex_color(CLR_LINE), 0);
        lv_obj_set_style_radius(pill, 8, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        set_pill_sid(pill, sid);

        char num[8];
        snprintf(num, sizeof(num), "%d", sid + 1);
        lv_obj_t* lbl = lv_label_create(pill);
        lv_label_set_text(lbl, num);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, hex_color(CLR_TEXT), 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(pill, ev_pill, LV_EVENT_CLICKED, nullptr);
        stn_pill_lbls[g_pill_count] = lbl;
        stn_pills[g_pill_count++]   = pill;
    }
}

// ---------------------------------------------------------------------------
// Sync LVGL widgets from the current PanelView
// ---------------------------------------------------------------------------
static void ui_update() {
    if (!g_ps) return;
    const osp::PanelView& v = g_ps->view();
    char buf[64];

    bool show_syncing = g_ps->pending_sync() && !g_ps->sync_stale();
    const bool running = (v.phase == osp::Phase::Running);
    const char* status_text = "Connected";
    uint32_t status_color = CLR_TEAL;
    if (show_syncing) {
        status_text = "Syncing...";
        status_color = CLR_AMBER;
    } else if (v.link == osp::LinkState::AuthError) {
        status_text = "Auth error";
        status_color = CLR_RED;
    } else if (v.link == osp::LinkState::Offline) {
        status_text = "Controller offline";
        status_color = CLR_RED;
    } else if (v.link == osp::LinkState::Reconnecting) {
        status_text = "Reconnecting...";
        status_color = CLR_AMBER;
    } else if (running) {
        status_text = "Running";
        status_color = CLR_TEAL;
    }

    // Link-state-appropriate glyph for status bar and idle banner (Part 4).
    const char* link_sym = LV_SYMBOL_WIFI;
    if (!show_syncing) {
        if (v.link == osp::LinkState::Reconnecting) link_sym = LV_SYMBOL_REFRESH;
        else if (v.link == osp::LinkState::Offline ||
                 v.link == osp::LinkState::AuthError) link_sym = LV_SYMBOL_WARNING;
    }

    snprintf(buf, sizeof(buf), "%s %s", link_sym, status_text);
    lv_label_set_text(lbl_host, buf);
    lv_obj_set_style_text_color(lbl_host, hex_color(status_color), 0);

    // Drawn RSSI bar meters (Part 3).
    update_sig_meter(sig_panel, osp::rssi_to_bars(WiFi.RSSI()),
                     WiFi.status() == WL_CONNECTED);
    update_sig_meter(sig_ctrl, osp::rssi_to_bars(v.ctrl_rssi),
                     v.link == osp::LinkState::Connected);

    // Phase visibility
    obj_set_hidden(pnl_idle,    running);
    obj_set_hidden(pnl_running, !running);
    obj_set_hidden(btn_advance, !running);
    obj_set_hidden(btn_stop,    !running);

    if (!running) {
        if (show_syncing) {
            snprintf(buf, sizeof(buf), "%s Syncing...", LV_SYMBOL_REFRESH);
            lv_label_set_text(lbl_idle_head, buf);
            lv_label_set_text(lbl_idle_sub, "Waiting for the controller to confirm");
            lv_obj_set_style_text_color(lbl_idle_sub, hex_color(CLR_AMBER), 0);
        } else if (v.link == osp::LinkState::AuthError) {
            snprintf(buf, sizeof(buf), "%s Auth error", LV_SYMBOL_WARNING);
            lv_label_set_text(lbl_idle_head, buf);
            snprintf(buf, sizeof(buf), "Invalid credentials for controller at %s",
                     g_os_host.isEmpty() ? "controller" : g_os_host.c_str());
            lv_label_set_text(lbl_idle_sub, buf);
            lv_obj_set_style_text_color(lbl_idle_sub, hex_color(CLR_RED), 0);
        } else if (v.link == osp::LinkState::Offline) {
            snprintf(buf, sizeof(buf), "%s Controller offline", LV_SYMBOL_WARNING);
            lv_label_set_text(lbl_idle_head, buf);
            snprintf(buf, sizeof(buf), "Cannot reach controller at %s",
                     g_os_host.isEmpty() ? "controller" : g_os_host.c_str());
            lv_label_set_text(lbl_idle_sub, buf);
            lv_obj_set_style_text_color(lbl_idle_sub, hex_color(CLR_RED), 0);
        } else if (v.link == osp::LinkState::Reconnecting) {
            snprintf(buf, sizeof(buf), "%s Reconnecting...", LV_SYMBOL_REFRESH);
            lv_label_set_text(lbl_idle_head, buf);
            lv_label_set_text(lbl_idle_sub, "Waiting for the controller to respond");
            lv_obj_set_style_text_color(lbl_idle_sub, hex_color(CLR_AMBER), 0);
        } else if (!v.station_list_loaded) {
            lv_label_set_text(lbl_idle_head, "Loading stations...");
            lv_label_set_text(lbl_idle_sub, "Waiting for the controller to respond");
            lv_obj_set_style_text_color(lbl_idle_sub, hex_color(CLR_MUTED), 0);
        } else {
            snprintf(buf, sizeof(buf), "%s Select a station", LV_SYMBOL_DOWN);
            lv_label_set_text(lbl_idle_head, buf);
            lv_label_set_text(lbl_idle_sub, "Tap a station below to start");
            lv_obj_set_style_text_color(lbl_idle_sub, hex_color(CLR_TEAL), 0);
        }
    }

    if (running) {
        // Eyebrow: "Station N"
        snprintf(buf, sizeof(buf), "STATION %d", v.running_sid + 1);
        lv_label_set_text(lbl_eyebrow, buf);

        // Station name
        const auto& stns = g_model.stations();
        const char* name = (v.running_sid >= 0 &&
                            v.running_sid < static_cast<int>(stns.size()))
                            ? stns[v.running_sid].name.c_str() : "\xe2\x80\x94";
        lv_label_set_text(lbl_stn_name, name);

        // Countdown (MM:SS)
        char cd[16];
        fmt_countdown(cd, v.countdown_s);
        lv_label_set_text(lbl_countdown, cd);
    }

    // Run-time value
    snprintf(buf, sizeof(buf), "%d:%02d", v.run_time_s / 60, v.run_time_s % 60);
    lv_label_set_text(lbl_rt_value, buf);

    // Auto-advance switch
    const bool sw_on = lv_obj_has_state(sw_auto_adv, LV_STATE_CHECKED);
    if (v.auto_advance != sw_on) {
        if (v.auto_advance) lv_obj_add_state(sw_auto_adv, LV_STATE_CHECKED);
        else                lv_obj_clear_state(sw_auto_adv, LV_STATE_CHECKED);
    }

    // Grid label (kept consistent as "STATIONS" in both idle and running).
    lv_label_set_text(lbl_grid_title, "STATIONS");

    // Pill highlights
    for (int i = 0; i < g_pill_count; ++i) {
        if (!stn_pills[i]) continue;
        const bool active = running && (get_pill_sid(stn_pills[i]) == v.running_sid);
        lv_obj_set_style_bg_color(stn_pills[i],
            hex_color(active ? CLR_TEAL : CLR_LINE), 0);
        if (stn_pill_lbls[i]) {
            lv_obj_set_style_text_color(stn_pill_lbls[i],
                hex_color(active ? CLR_BG : CLR_TEXT), 0);
        }
    }

    // Sleep overlay + backlight
    obj_set_hidden(sleep_overlay, !v.sleeping);
    ledcWrite(LEDC_CHANNEL, v.sleeping ? BACKLIGHT_OFF : BACKLIGHT_ON);
}

// ---------------------------------------------------------------------------
// HTTP transport
// ---------------------------------------------------------------------------
static osp::Transport make_http_transport() {
    return [](const std::string& url) -> std::string {
        HTTPClient http;
        http.setConnectTimeout(1500);
        http.setTimeout(1500);
        if (!http.begin(url.c_str())) return "";
        const int code = http.GET();
        if (code != HTTP_CODE_OK) { http.end(); return ""; }
        const String body = http.getString();
        http.end();
        return body.c_str();
    };
}

static void apply_link_error(uint32_t now_ms, int* failures) {
    if (!g_ps) return;
    ++(*failures);
    StateLock lock;
    if (!lock || !g_ps) return;
    g_ps->mark_desired_needs_retry();
    if (g_client && g_client->last_result() == osp::OsResult::Unauthorized) {
        g_ps->on_auth_error(now_ms);
    } else if (*failures >= LINK_RETRY_LIMIT) {
        g_ps->on_link_offline(now_ms);
    } else {
        g_ps->on_link_reconnecting(now_ms);
    }
    cache_phase_snapshot_unlocked();
}

static bool refresh_station_list(uint32_t now_ms, int* failures) {
    if (!g_client) return false;

    osp::JnData jn;
    if (!g_client->fetch_jn(jn)) {
        apply_link_error(now_ms, failures);
        StateLock lock;
        if (lock && g_ps) g_ps->set_station_list_loaded(false);
        return false;
    }

    osp::JoData jo;
    g_client->fetch_jo(jo);

    {
        StateLock lock;
        if (!(lock && g_ps)) return false;
        g_model.load(jn.snames, jn.stn_dis, jo.mas, jo.mas2);
        ++g_model_version;
        g_ps->set_station_list_loaded(true);
        g_ps->on_link_connected(now_ms);
        cache_phase_snapshot_unlocked();
    }

    *failures = 0;
    Serial.printf("Stations: %d total, %d runnable (mas=%d mas2=%d)\n",
                  static_cast<int>(jn.snames.size()),
                  g_model.runnable_count(), jo.mas, jo.mas2);
    return true;
}

static bool deliver_desired(uint32_t now_ms, int* failures) {
    if (!g_client) return false;

    osp::DesiredIntent desired;
    osp::Phase phase = osp::Phase::Idle;
    int running_sid = -1;
    bool can_deliver = false;
    {
        StateLock lock;
        if (!(lock && g_ps)) return false;
        desired = g_ps->desired();
        phase = g_ps->view().phase;
        running_sid = g_ps->view().running_sid;
        can_deliver = g_ps->can_deliver_desired();
    }
    if (!can_deliver) return false;

    bool ok = false;
    if (desired.kind == osp::IntentKind::Stop) {
        ok = g_client->stop_all();
    } else if (desired.kind == osp::IntentKind::Run) {
        if (phase == osp::Phase::Running && running_sid >= 0) {
            if (running_sid == desired.sid) {
                ok = g_client->extend(running_sid, desired.seconds);
            } else {
                ok = g_client->advance(running_sid, desired.sid, desired.seconds);
            }
        } else {
            ok = g_client->run_station(desired.sid, desired.seconds);
        }
    }

    if (!ok) {
        apply_link_error(now_ms, failures);
        return false;
    }

    {
        StateLock lock;
        if (lock && g_ps) {
            g_ps->mark_desired_delivered();
            g_ps->on_link_connected(now_ms);
            cache_phase_snapshot_unlocked();
        }
    }
    *failures = 0;
    return true;
}

static bool poll_controller(uint32_t now_ms, int* failures) {
    if (!g_client) return false;
    osp::JcData jc;
    if (!g_client->fetch_jc(jc)) {
        apply_link_error(now_ms, failures);
        return false;
    }

    {
        StateLock lock;
        if (lock && g_ps) {
            g_ps->on_jc(jc, now_ms);
            cache_phase_snapshot_unlocked();
        }
    }
    *failures = 0;
    return true;
}

static void ui_task(void* /*arg*/) {
    uint32_t last_lv_tick_ms = millis();
    uint32_t seen_model_version = UINT32_MAX;

    for (;;) {
        ++g_ui_beat;
        const uint32_t now = millis();
        const uint32_t delta = now - last_lv_tick_ms;
        if (delta >= UI_TICK_MS) {
            lv_tick_inc(delta);
            last_lv_tick_ms = now;
        }

        {
            StateLock lock;
            if (lock && g_ps) {
                g_ps->tick(now);
                cache_phase_snapshot_unlocked();
                if (seen_model_version != g_model_version) {
                    build_grid();
                    seen_model_version = g_model_version;
                }
                ui_update();
            }
        }

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
    }
}

static void network_task(void* /*arg*/) {
    uint32_t next_jn_attempt_ms = 0;
    uint32_t jn_retry_ms = JN_RETRY_INITIAL_MS;
    uint32_t last_jc_poll_ms = 0;
    int link_failures = 0;
    bool station_list_loaded = false;

    for (;;) {
        ++g_net_beat;
        const uint32_t now = millis();

        dev_loop_handle();

        if (WiFi.status() != WL_CONNECTED || !g_client) {
            StateLock lock;
            if (lock && g_ps) {
                g_ps->on_link_offline(now);
                cache_phase_snapshot_unlocked();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!station_list_loaded && now >= next_jn_attempt_ms) {
            if (refresh_station_list(now, &link_failures)) {
                station_list_loaded = true;
                jn_retry_ms = JN_RETRY_INITIAL_MS;
            } else {
                next_jn_attempt_ms = now + jn_retry_ms;
                jn_retry_ms = std::min<uint32_t>(jn_retry_ms * 2, JN_RETRY_MAX_MS);
            }
        }

        bool poll_now = false;
        bool desired_delivered = false;
        bool pending_sync = false;
        {
            StateLock lock;
            if (lock && g_ps) {
                desired_delivered = g_ps->desired_delivered();
                pending_sync = g_ps->pending_sync();
            }
        }

        if (deliver_desired(now, &link_failures)) {
            poll_now = true;
        }

        if (pending_sync || desired_delivered ||
            (now - last_jc_poll_ms) >= JC_POLL_INTERVAL_MS) {
            poll_now = true;
        }

        if (poll_now && poll_controller(now, &link_failures)) {
            last_jc_poll_ms = now;
            bool redeliver = false;
            {
                StateLock lock;
                if (lock && g_ps) {
                    redeliver = !g_ps->desired_delivered() && g_ps->pending_sync();
                }
            }
            if (redeliver && deliver_desired(now, &link_failures)) {
                last_jc_poll_ms = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(NETWORK_LOOP_MS));
    }
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("OpenSprinkler panel — M6 booting");

    // Backlight via LEDC PWM (GPIO27 per docs/03).
    ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(PIN_BACKLIGHT, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, BACKLIGHT_ON);

    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

    // ---- TFT init -------------------------------------------------------
    tft.init();
    // LVGL 9 renders RGB565 in the ESP32's native little-endian byte order, but
    // tft.pushPixels() sends 16-bit words MSB-first — so LVGL buffers must be
    // byte-swapped or colours come out wrong (near-black bg renders red-ish,
    // anti-aliased text edges turn to rainbow speckle). setSwapBytes only affects
    // buffer pushes (pushPixels/pushImage), NOT fillScreen/drawString, so raw
    // graphics are unaffected. Must be set before any LVGL flush.
    tft.setSwapBytes(true);
    tft.setRotation(1);  // landscape — tune rotation/offset on-device (docs/03)
    tft.fillScreen(TFT_BLACK);

    // ---- Load NVS config ------------------------------------------------
    int saved_rt = osp::PanelState::kDefaultRunTime;
    bool saved_aa = false;
    load_config_from_nvs(nullptr, nullptr, nullptr, nullptr, nullptr, &saved_rt, nullptr, &saved_aa);

    // ---- Provision / connect (M4 seam) ----------------------------------
    ensure_network_config();
    load_config_from_nvs(nullptr, nullptr, nullptr, nullptr, nullptr, &saved_rt, nullptr, &saved_aa);

    // ---- OTA responder + TCP log server (requires Wi-Fi) ----------------
    // ArduinoOTA.begin() only called when NVS ota_pass is non-empty.
    // WiFiServer.begin() only called when NVS dev_log is true.
    if (WiFi.status() == WL_CONNECTED) {
        String ota_pass_dl;
        bool dev_log_dl = false;
        load_config_from_nvs(nullptr, nullptr, nullptr, nullptr, &ota_pass_dl, nullptr, &dev_log_dl);
        dev_loop_init(ota_pass_dl, dev_log_dl);
    }

    // ---- Touch calibration -----------------------------------------------
    // Must run AFTER WiFi/provisioning (portal is phone-based, no panel touch
    // needed) and BEFORE lv_init() (calibrateTouch draws directly via TFT_eSPI).
    ensure_touch_calibration();

    // ---- LVGL init ------------------------------------------------------
    lv_init();
    lv_display_t* disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, draw_buf, nullptr,
                           sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchpad_read_cb);

    g_state_mutex = xSemaphoreCreateMutex();

    // ---- Build base UI --------------------------------------------------
    build_ui();
    lv_timer_handler();

    // ---- Init OsClient --------------------------------------------------
    const String host_url = "http://" + g_os_host;
    g_client.reset(new osp::OsClient(
        host_url.c_str(), g_pw_md5.c_str(), make_http_transport()));

    // ---- Init state machine + background tasks -------------------------
    {
        StateLock lock;
        if (lock) {
            g_ps.reset(new osp::PanelState(g_model, saved_rt));
            g_ps->set_auto_advance(saved_aa);
            cache_phase_snapshot_unlocked();
        }
    }
    build_grid();
    {
        StateLock lock;
        if (lock) {
            cache_phase_snapshot_unlocked();
            ui_update();
        }
    }

    xTaskCreatePinnedToCore(ui_task, "ui-task", 8192, nullptr, 2, &g_ui_task_handle, 1);
    xTaskCreatePinnedToCore(network_task, "net-task", 12288, nullptr, 1, &g_net_task_handle, 0);
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
    if (g_dev_log_enabled) {
        const uint32_t ui_beat = g_ui_beat;
        const uint32_t net_beat = g_net_beat;
        const uint32_t phase = g_phase_snapshot;
        const UBaseType_t ui_hwm =
            g_ui_task_handle ? uxTaskGetStackHighWaterMark(g_ui_task_handle) : 0;
        const UBaseType_t net_hwm =
            g_net_task_handle ? uxTaskGetStackHighWaterMark(g_net_task_handle) : 0;
        Serial.printf(HEARTBEAT_LOG_FORMAT,
                      static_cast<unsigned long>(millis()),
                      static_cast<unsigned long>(ui_beat),
                      static_cast<unsigned long>(net_beat),
                      phase_snapshot_name(phase),
                      static_cast<unsigned int>(ESP.getFreeHeap()),
                      static_cast<unsigned int>(ui_hwm),
                      static_cast<unsigned int>(net_hwm));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}
