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

#include "bench_probe.h"
#include "os_client.h"
#include "panel_config.h"
#include "panel_state.h"
#include "station_model.h"
#include "battery_monitor.h"
#include "ui_font_countdown_48.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int PIN_BACKLIGHT = 27;
static constexpr int PIN_BOOT_BTN  = 0;
// Battery sense: BAT_ADC on GPIO34 (ADC1_CH6, input-only, Wi-Fi-safe) reads
// VBAT/2 through an even 100K/100K divider (schematic E32R35T, ratio confirmed
// 1.991 on-device). See lib/battery_monitor.
static constexpr int PIN_BAT_ADC   = 34;
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
static constexpr int           PORTAL_EDIT_TIMEOUT_S = 600;
static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr int           CALIBRATION_COMPLETE_DELAY_MS = 1000;
static constexpr uint32_t UI_TICK_MS = 5;
static constexpr uint32_t NETWORK_LOOP_MS = 50;
static constexpr uint32_t JC_POLL_INTERVAL_MS = 2000;
// Battery: multisample count per read and how often to sample the ADC. The
// gauge is coarse and EMA-smoothed, so a slow cadence keeps it calm and cheap.
static constexpr int      BAT_ADC_SAMPLES  = 16;
static constexpr uint32_t BAT_SAMPLE_MS    = 3000;
static constexpr uint32_t JN_RETRY_INITIAL_MS = 1000;
static constexpr uint32_t JN_RETRY_MAX_MS = 10000;
static constexpr int LINK_RETRY_LIMIT = 3;
static constexpr const char* HEARTBEAT_LOG_FORMAT =
    "[HB] ms=%lu ui=%lu net=%lu phase=%s sleeping=%d idle_ms=%lu "
    "sleep_to_ms=%lu heap=%u ui_hwm=%u net_hwm=%u\n";
static constexpr uint32_t HEARTBEAT_LOG_INTERVAL_MS = 1000;

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
static constexpr const char* NVS_SLEEP     = "sleep_s";
// Idle-sleep timeout in seconds, persisted in NVS and settable in the config
// portal. 0 disables sleep. Default mirrors PanelState::kDefaultSleepTimeoutMs.
static constexpr int DEFAULT_SLEEP_S = 300;
static constexpr int MAX_SLEEP_S     = 3600;
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
static constexpr uint32_t CLR_TEALDIM = 0x1c6a64;  // accent rule / dim chip border
static constexpr uint32_t CLR_LEDE    = 0xc3d3cf;  // supporting body text

// Build an lv_color_t from a 0xRRGGBB constant.
static inline lv_color_t hex_color(uint32_t hex) {
    return lv_color_make((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

static inline void obj_set_hidden(lv_obj_t* obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Hardware objects
// ---------------------------------------------------------------------------
static TFT_eSPI tft;

// Convert a 0xRRGGBB constant to a TFT_eSPI 16-bit (RGB565) color.
static inline uint16_t tft565(uint32_t hex) {
    return tft.color565((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

// Firmware git SHA — injected by CI via -D FW_GIT_SHA (GIT_SHA env var at
// build time). Falls back to "dev" when unset (empty string from sysenv).
#ifndef FW_GIT_SHA
#  define FW_GIT_SHA ""
#endif
static inline const char* fw_git_sha() {
    return (FW_GIT_SHA[0] != '\0') ? FW_GIT_SHA : "dev";
}

// Firmware version — injected by the release workflow via -D FW_VERSION
// (FW_VERSION env var = the git tag at build time). Falls back to "dev" when
// unset (empty string from sysenv), matching the fw_git_sha() pattern.
#ifndef FW_VERSION
#  define FW_VERSION ""
#endif
static inline const char* fw_version() {
    return (FW_VERSION[0] != '\0') ? FW_VERSION : "dev";
}

// Boot brand tag shown on the boot screens: "<version> (<sha>)" for a tagged
// release build, or just the bare sha/"dev" when no version is set. Uses a
// static buffer — called from the single-threaded boot draw path and the
// captive-portal header builder (also synchronous/single-threaded during
// setup/config), so the static buffer remains safe.
static inline const char* fw_boot_tag() {
    static char buf[48];
    if (strcmp(fw_version(), "dev") != 0) {
        snprintf(buf, sizeof(buf), "%s (%s)", fw_version(), fw_git_sha());
        return buf;
    }
    return fw_git_sha();
}

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
// Touch trace edge-tracking (#60 observability): remember whether the last
// poll was pressed so we can emit one [TOUCH] dev-log line per press (rising
// edge) without spamming every poll. Single-sample phantom presses no longer
// keep the panel awake — that was really the enter_idle() re-affirm resetting
// the idle timer on every /jc poll (fixed in panel_state), not raw touch — so
// no debounce is applied here and single taps stay instant/responsive.
static bool g_touch_was_pressed = false;
// Snapshots for the heartbeat (fix #60 observability): populated under the
// state lock in ui_task so loop() can print them without taking the lock.
static volatile bool     g_sleeping_snapshot   = false;
static volatile uint32_t g_idle_ms_snapshot    = 0;
static volatile uint32_t g_sleep_to_ms_snapshot = 0;
static volatile int g_batt_override_percent = -1;
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
        g_sleeping_snapshot = g_ps->view().sleeping;
        g_idle_ms_snapshot = g_ps->idle_elapsed_ms();
        g_sleep_to_ms_snapshot = g_ps->sleep_timeout_ms();
    }
}

static const char* phase_snapshot_name(uint32_t phase) {
    switch (static_cast<osp::Phase>(phase)) {
        case osp::Phase::Idle:
            return "Idle";
        case osp::Phase::Running:
            return "Running";
        case osp::Phase::ProgramRunning:
            return "ProgramRunning";
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

// --- Bench probe: on-demand screen capture + synthetic touch over :2323 ------
// Lets a bench host pull a pixel-exact screenshot and drive the UI without
// physical touch (gated behind dev_log, like the log stream itself). See
// lib/bench_probe for the wire protocol.
//
// Threading: the dev-log socket is serviced on network_task (core 0), but LVGL
// (disp_flush_cb / touchpad_read_cb / lv_timer_handler) runs on the UI task.
// LVGL is not thread-safe and WiFiClient writes must not race across cores
// (fix #79), so capture is split: the net task only *requests* a capture and
// stops writing to the client (g_capture_suppress_log); the UI task performs
// the lv_obj_invalidate and is then the SOLE writer of the framebuffer bytes
// for the duration of the frame. Touch injection is an SPSC ring: net task
// produces, UI task consumes.
static volatile bool g_capture_request     = false;  // net task -> UI task
static volatile bool g_capture_active      = false;  // UI task owns the send
static volatile bool g_capture_suppress_log = false; // silence the log tee

// Write every byte of a capture frame to the dev-log client, retrying on short
// or zero-length writes. Arduino-ESP32 WiFiClient::write returns fewer bytes
// than requested (or 0) when lwIP's TCP send buffer is momentarily full. During
// a screenshot the ~300 KB of strip data survives because the slow SPI
// pushPixels between strips lets the tx buffer drain, but the tiny \x02END\n
// terminator is written immediately after the final strip with no drain gap —
// so a single ignored short write silently dropped it, leaving the host client
// blocked waiting for an END that never arrived. Retry until every byte is
// accepted (bounded by a deadline + connection check so a dead socket can't
// wedge the UI task).
static bool capture_write_all(const uint8_t* buf, size_t n) {
    size_t sent = 0;
    const uint32_t deadline = millis() + 2000;
    while (sent < n) {
        if (!g_log_client || !g_log_client.connected()) return false;
        const size_t w = g_log_client.write(buf + sent, n - sent);
        sent += w;
        if (w == 0) {
            if (static_cast<int32_t>(millis() - deadline) >= 0) return false;
            delay(1);  // let the tx buffer drain, then retry
        }
    }
    return true;
}

struct InjTouch { int16_t x; int16_t y; bool pressed; };
static constexpr int INJ_QUEUE_LEN = 8;
static InjTouch      g_inj_queue[INJ_QUEUE_LEN];
static volatile int  g_inj_head = 0;  // next write slot (producer: net task)
static volatile int  g_inj_tail = 0;  // next read slot  (consumer: UI task)

static bool inj_push(int16_t x, int16_t y, bool pressed) {
    const int next = (g_inj_head + 1) % INJ_QUEUE_LEN;
    if (next == g_inj_tail) return false;  // full — drop
    g_inj_queue[g_inj_head] = {x, y, pressed};
    g_inj_head = next;
    return true;
}
static bool inj_pop(InjTouch* out) {
    if (g_inj_tail == g_inj_head) return false;  // empty
    *out = g_inj_queue[g_inj_tail];
    g_inj_tail = (g_inj_tail + 1) % INJ_QUEUE_LEN;
    return true;
}
// TeeSerial: Print subclass that writes to UART0 and, when a TCP log client is
// connected, also to that client.  Keeps the per-character path tiny to avoid
// budget pressure on the no-PSRAM ESP32.
class TeeSerial : public Print {
public:
    void begin(unsigned long baud) { g_hw_serial->begin(baud); }
    int  availableForWrite()       { return g_hw_serial->availableForWrite(); }

    size_t write(uint8_t c) override {
        g_hw_serial->write(c);
        if (!g_capture_suppress_log && g_log_client && g_log_client.connected()) {
            g_log_client.write(c);
        }
        return 1;
    }
    size_t write(const uint8_t* buf, size_t n) override {
        g_hw_serial->write(buf, n);
        if (!g_capture_suppress_log && g_log_client && g_log_client.connected()) {
            g_log_client.write(buf, n);
        }
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

static void handle_bench_command(const char* line) {
    const bench::Command c = bench::parse_command(line);
    switch (c.cmd) {
        case bench::Cmd::Shot:
            // Ask the UI task to render + stream a frame. Suppress the log tee
            // now (on this task) so nothing interleaves the binary stream once
            // the UI task starts writing it.
            g_capture_suppress_log = true;
            g_capture_request = true;
            break;
        case bench::Cmd::Tap:
            inj_push(c.x, c.y, true);
            inj_push(c.x, c.y, false);
            break;
        case bench::Cmd::Down:
        case bench::Cmd::Move:
            inj_push(c.x, c.y, true);
            break;
        case bench::Cmd::Up:
            inj_push(0, 0, false);
            break;
        case bench::Cmd::Battery:
            g_batt_override_percent = c.x;
            break;
        case bench::Cmd::BatteryAuto:
            g_batt_override_percent = -1;
            break;
        case bench::Cmd::Invalid:
            g_hw_serial->printf("[BENCH] ignoring bad command: %s\n", line);
            break;
        case bench::Cmd::None:
        default:
            break;
    }
}

static void dev_loop_handle() {
    if (g_ota_started) {
        ArduinoOTA.handle();
    }

    // While a screen capture is streaming, the UI task is the sole owner of the
    // log socket (see the bench-probe threading note). Skip ALL socket
    // maintenance here so the net task never races it cross-core mid-frame.
    if (g_log_server_started && !g_capture_active) {
        // Accept new TCP log client (single-slot: drops any stale connection).
        if (g_log_server.hasClient()) {
            if (g_log_client) g_log_client.stop();
            g_log_client = g_log_server.accept();
            // Disable Nagle so the tiny trailing \x02END\n frame is pushed
            // immediately after the strip burst instead of being held waiting
            // for an ACK of the ~300 KB that preceded it.
            g_log_client.setNoDelay(true);
            g_hw_serial->println("[LOG] client connected");
            g_log_client.println("[LOG] OSPanel log stream");
        }
        // Silently drop dead connections so write() doesn't block. If the
        // client vanished mid-capture, release the log suppression so UART0
        // logging resumes and a wedged screenshot can't silence the stream.
        if (g_log_client && !g_log_client.connected()) {
            g_log_client.stop();
            g_capture_request = false;
            g_capture_active = false;
            g_capture_suppress_log = false;
        }

        // Drain inbound bench-probe commands (one per line).
        if (g_log_client && g_log_client.connected()) {
            static char linebuf[64];
            static size_t linelen = 0;
            while (g_log_client.available()) {
                const char ch = static_cast<char>(g_log_client.read());
                if (ch == '\r') continue;
                if (ch == '\n') {
                    linebuf[linelen] = '\0';
                    handle_bench_command(linebuf);
                    linelen = 0;
                    if (g_capture_request) break;  // let the UI task take over
                } else if (linelen < sizeof(linebuf) - 1) {
                    linebuf[linelen++] = ch;
                } else {
                    linelen = 0;  // overlong line — drop it
                }
            }
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

// ---------------------------------------------------------------------------
// Boot screens (pre-LVGL, drawn directly via TFT_eSPI).
//
// Consistent layout echoing the app: a teal mono eyebrow wordmark + accent
// rule, a large headline, optional supporting content, and a muted footer.
// All primitives (text/rects/rules) render with the fonts loaded in
// platformio.ini (GLCD/FONT2/FONT4). 480x320 landscape.
// ---------------------------------------------------------------------------
static constexpr int BOOT_MX = 28;    // left content margin
static constexpr int BOOT_RW = 424;   // content width (480 - 2*BOOT_MX)

// Fill the background, draw the wordmark eyebrow + git-SHA suffix, and the
// accent rule. `tag` is the muted suffix after the wordmark (the firmware SHA);
// the mode is conveyed by the larger headline below, so the eyebrow stays
// consistent across every boot screen.
static void draw_boot_chrome(const char* tag, uint32_t rule_hex) {
    const uint16_t bg = tft565(CLR_BG);
    tft.fillScreen(bg);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(tft565(CLR_TEAL), bg);
    const int w = tft.drawString("OPENSPRINKLER PANEL", BOOT_MX, 24);
    if (tag && tag[0]) {
        tft.setTextColor(tft565(CLR_MUTED), bg);
        tft.drawString(String("   ") + tag, BOOT_MX + w, 24);
    }
    tft.fillRect(BOOT_MX, 48, 70, 3, tft565(rule_hex));
}

static void draw_boot_headline(const char* text, uint32_t color_hex) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(tft565(color_hex), tft565(CLR_BG));
    tft.drawString(text, BOOT_MX, 80);
}

static void draw_boot_lede(const char* text, int y) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(tft565(CLR_LEDE), tft565(CLR_BG));
    tft.drawString(text, BOOT_MX, y);
}

// Prominent value line (SSID / URL / IP).
static void draw_boot_value(const char* text, int y, uint32_t color_hex) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(tft565(color_hex), tft565(CLR_BG));
    tft.drawString(text, BOOT_MX, y);
}

static void draw_boot_footer(const char* text, int y = 292) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(tft565(CLR_MUTED), tft565(CLR_BG));
    tft.drawString(text, BOOT_MX, y);
}

// Numbered setup step: a teal-outlined circle + label.
static void draw_boot_step(int n, const char* text, int y) {
    const uint16_t bg = tft565(CLR_BG);
    const int cx = BOOT_MX + 14, cy = y + 14;
    tft.drawCircle(cx, cy, 14, tft565(CLR_TEALDIM));
    tft.drawCircle(cx, cy, 13, tft565(CLR_TEALDIM));
    char ns[4];
    snprintf(ns, sizeof(ns), "%d", n);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(tft565(CLR_TEAL), bg);
    tft.drawString(ns, cx, cy);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(tft565(CLR_LEDE), bg);
    tft.drawString(text, BOOT_MX + 42, y + 3);
}

// Boot-hold tier row: a duration chip + label. Highlighted when `active`.
static void draw_boot_tier(int y, const char* dur, const char* label,
                           uint32_t accent_hex, bool active) {
    const uint16_t bg = tft565(CLR_BG);
    const uint16_t box = active ? tft565(accent_hex) : tft565(CLR_LINE);
    const uint16_t durc = active ? tft565(accent_hex) : tft565(CLR_MUTED);
    tft.drawRoundRect(BOOT_MX, y, 66, 30, 6, box);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(durc, bg);
    tft.drawString(dur, BOOT_MX + 33, y + 15);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(active ? tft565(accent_hex) : tft565(CLR_LEDE), bg);
    tft.drawString(label, BOOT_MX + 82, y + 3);
}

// Static hold-progress bar (fraction of the 10 s factory threshold).
static void draw_boot_hold_bar(int pct, uint32_t color_hex) {
    const int x = BOOT_MX, y = 300, h = 8;
    tft.fillRoundRect(x, y, BOOT_RW, h, 4, tft565(CLR_LINE));
    const int fw = (BOOT_RW * pct) / 100;
    if (fw > 3) tft.fillRoundRect(x, y, fw, h, 4, tft565(color_hex));
}

// ---- Composed boot screens ------------------------------------------------
static void draw_boot_connecting(const char* ssid) {
    draw_boot_chrome(fw_boot_tag(), CLR_TEALDIM);
    draw_boot_headline("Connecting", CLR_TEXT);
    draw_boot_lede("Joining", 134);
    draw_boot_value(ssid, 158, CLR_TEXT);
    draw_boot_footer("Timeout 15 s, then setup mode");
}

static void draw_boot_setup(const char* ap_ssid) {
    draw_boot_chrome(fw_boot_tag(), CLR_TEALDIM);
    draw_boot_headline("Set up your panel", CLR_TEXT);
    draw_boot_step(1, (String("Join Wi-Fi ") + ap_ssid).c_str(), 138);
    draw_boot_step(2, "Open 192.168.4.1", 186);
    draw_boot_footer("Waiting for a device to connect");
}

static void draw_boot_configure(const char* url) {
    draw_boot_chrome(fw_boot_tag(), CLR_TEALDIM);
    draw_boot_headline("Configure", CLR_TEXT);
    draw_boot_lede("Open on your network:", 134);
    draw_boot_value(url, 160, CLR_TEAL);
    draw_boot_footer("Times out in 10 min");
}

static void draw_boot_hold(BootMode mode) {
    const bool edit = (mode != BootMode::kNormal);
    draw_boot_chrome(fw_boot_tag(), CLR_TEALDIM);
    draw_boot_headline("Keep holding BOOT", CLR_TEXT);
    draw_boot_tier(150, "3 s",  "Configure",     CLR_TEAL, edit);
    draw_boot_tier(192, "10 s", "Factory reset", CLR_RED,  false);
    draw_boot_footer(edit ? "Release now to configure" : "Hold to reconfigure", 272);
    draw_boot_hold_bar(edit ? 33 : 0, CLR_TEAL);
}

static void draw_boot_factory() {
    draw_boot_chrome(fw_boot_tag(), CLR_RED);
    draw_boot_headline("Erasing all settings", CLR_RED);
    draw_boot_lede("Clearing Wi-Fi, OpenSprinkler & touch calibration.", 134);
    draw_boot_footer("The panel will restart in setup mode");
}

static void draw_boot_notice(const char* headline) {
    draw_boot_chrome(fw_boot_tag(), CLR_TEALDIM);
    draw_boot_headline(headline, CLR_TEXT);
}

static void load_config_from_nvs(String* ssid,
                                 String* pass,
                                 String* os_host,
                                 String* pw_md5,
                                 String* ota_pass,
                                 int* run_time_s,
                                 bool* dev_log = nullptr,
                                 bool* auto_advance = nullptr,
                                 int* sleep_timeout_s = nullptr) {
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
    if (sleep_timeout_s) *sleep_timeout_s = prefs.getInt(NVS_SLEEP, DEFAULT_SLEEP_S);
    prefs.end();
}

static void save_config_to_nvs(const String& ssid,
                               const String& pass,
                               const String& os_host,
                               const String& pw_md5,
                               const String& ota_pass,
                               bool dev_log,
                               int sleep_timeout_s) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putString(NVS_SSID, ssid);
    prefs.putString(NVS_PASS, pass);
    prefs.putString(NVS_HOST, os_host);
    prefs.putString(NVS_PWMD5, pw_md5);
    if (ota_pass.isEmpty()) prefs.remove(NVS_OTA);
    else                    prefs.putString(NVS_OTA, ota_pass);
    prefs.putBool(NVS_DEV_LOG, dev_log);
    prefs.putInt(NVS_SLEEP, sleep_timeout_s);
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

    draw_boot_hold(BootMode::kNormal);

    const unsigned long t0 = millis();
    BootMode mode = BootMode::kNormal;

    while (digitalRead(PIN_BOOT_BTN) == LOW) {
        const unsigned long held = millis() - t0;
        if (mode == BootMode::kNormal && held >= BOOT_HOLD_EDIT_MS) {
            mode = BootMode::kEditConfig;
            draw_boot_hold(BootMode::kEditConfig);
        }
        if (mode == BootMode::kEditConfig && held >= BOOT_HOLD_FACTORY_MS) {
            mode = BootMode::kFactoryClear;
            draw_boot_factory();
            break;
        }
        delay(10);
    }
    return mode;
}

static bool connect_wifi(const String& ssid, const String& pass) {
    if (ssid.isEmpty()) return false;
    Serial.printf("Connecting to %s\n", ssid.c_str());
    draw_boot_connecting(ssid.c_str());
    WiFi.mode(WIFI_STA);
    // Disable WiFi modem-sleep (Arduino-ESP32 enables WIFI_PS_MIN_MODEM by
    // default). This is a wall-powered panel, so radio stability beats the tiny
    // idle-current saving: DTIM-beacon modem-sleep is the ESPHome
    // `power_save_mode` knob that causes latency spikes / jitter / dropped
    // frames on some APs (e.g. UniFi). Keep the link always-on and responsive.
    WiFi.setSleep(false);
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
                                      const WiFiManagerParameter& dev_log_param,
                                      const WiFiManagerParameter& sleep_param) {
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

    // Idle-sleep timeout (seconds). Blank/invalid falls back to the default;
    // clamp to [0, MAX_SLEEP_S] (0 = never sleep).
    const String sleep_str = osp::trim_ascii(sleep_param.getValue()).c_str();
    int saved_sleep_s = sleep_str.isEmpty() ? DEFAULT_SLEEP_S : sleep_str.toInt();
    if (saved_sleep_s < 0) saved_sleep_s = 0;
    if (saved_sleep_s > MAX_SLEEP_S) saved_sleep_s = MAX_SLEEP_S;

    save_config_to_nvs(ssid, pass, saved_host, saved_pw_md5, saved_ota_pass,
                       saved_dev_log, saved_sleep_s);
    Serial.printf("Config saved for host %s (pw_md5 %s, ota_pass %s, dev_log %s, sleep_s %d)\n",
                  saved_host.c_str(),
                  osp::is_valid_md5_hex(saved_pw_md5.c_str()) ? "set" : "invalid",
                  saved_ota_pass.isEmpty() ? "empty" : "set",
                  saved_dev_log ? "true" : "false", saved_sleep_s);
}

// ---------------------------------------------------------------------------
// Web-portal theming (shared by the AP captive portal and the STA edit portal).
//
// WiFiManager injects _customHeadElement into every page's <head> AFTER its own
// stylesheet, so a trailing <style> block re-skins all pages (menu, wifi, info,
// exit) to match the app + boot screens. setCustomHeadElement / setCustomMenuHTML
// keep the POINTER (not a copy), so both are backed by static storage.
// ---------------------------------------------------------------------------

// #44: replace the stock "Configure WiFi" menu button with "Configure" (the
// page now covers Wi-Fi + OpenSprinkler + panel options). Links to /wifi.
// The trailing <br/> matches the stock HTTP_PORTAL_MENU items (getMenuOut
// appends _customMenuHTML verbatim) so this button gets the same vertical gap
// as the others instead of butting into the next button.
static const char PORTAL_MENU_HTML[] =
    "<form action='/wifi' method='get'><button>Configure</button></form><br/>";

// Built once (embeds the firmware SHA in the brand line) and kept alive for the
// portal's lifetime.
static const char* portal_head_css() {
    static String css;
    if (css.length() == 0) {
        css.reserve(1500);
        css  = F("<style>");
        css += F("body{background:#07100f!important;color:#e9f2ef!important;font-family:"
                 "-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif!important}");
        css += F(".wrap{max-width:460px;padding:4px 14px 24px}");
        // Consistent brand header on every page; hide the stock root <h1> so it
        // isn't duplicated on the menu page.
        css += F("h1{display:none}");
        css += F(".wrap::before{display:block;content:'OpenSprinkler Panel · ");
        css += fw_boot_tag();
        css += F("';font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;"
                 "letter-spacing:.14em;color:#35d0c3;text-align:center;padding:16px 0 13px;"
                 "border-bottom:2px solid #1c6a64;margin:0 0 18px}");
        css += F("h3{color:#e9f2ef;text-align:left}"
                 "hr{border:0;border-top:1px solid rgba(233,247,243,.10);margin:14px 0}");
        css += F("button,input[type=submit],input[type=button]{background:#35d0c3!important;"
                 "color:#07100f!important;border:0!important;border-radius:10px!important;"
                 "font-weight:600!important;line-height:2.4rem!important;font-size:1.05rem!important}");
        css += F("input,select,textarea{background:#131d1c!important;color:#e9f2ef!important;"
                 "border:1px solid rgba(233,247,243,.12)!important;border-radius:9px!important}");
        css += F("input[type=checkbox],input[type=radio]{width:auto;accent-color:#35d0c3}");
        css += F("a{color:#35d0c3!important;font-weight:600}");
        css += F(".msg{background:#182322!important;color:#c3d3cf!important;"
                 "border:1px solid rgba(233,247,243,.10)!important;"
                 "border-left:3px solid #35d0c3!important;border-radius:8px!important}");
        css += F(".msg.D{border-left-color:#ff5b5b!important}");
        // Scan-list rows + signal icons (invert the dark PNG sprite for dark bg).
        css += F(".q{color:#7f938f}.q:before,.q:after{filter:invert(1)}");
        // /info definition lists + routes table.
        css += F("dt{color:#7f938f;font-weight:400}"
                 "dd{color:#e9f2ef;font-family:ui-monospace,Menlo,Consolas,monospace}");
        css += F("th{color:#35d0c3;text-align:left;border-bottom:1px solid rgba(233,247,243,.10)}"
                 "td{border-bottom:1px solid rgba(233,247,243,.08)}");
        css += F("</style>");
    }
    return css.c_str();
}

// Apply the brand title, themed CSS, and #44 menu rename to a portal instance.
static void apply_portal_theme(WiFiManager& wm) {
    wm.setTitle("OpenSprinkler Panel");
    wm.setCustomHeadElement(portal_head_css());
    static const char* kMenu[] = {"custom", "info", "update", "exit"};
    wm.setMenu(kMenu, 4);
    wm.setCustomMenuHTML(PORTAL_MENU_HTML);
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
                                      int current_sleep_s,
                                      bool non_destructive,
                                      bool* touch_cal_reset_out) {
    draw_boot_setup(PROVISION_AP_SSID);

    char host_buf[65] = {};
    char ota_buf[65] = {};
    char sleep_buf[8] = {};
    String normalized_host = osp::normalize_os_host(current_host.c_str()).c_str();
    String trimmed_ota = osp::trim_ascii(current_ota_pass.c_str()).c_str();
    normalized_host.toCharArray(host_buf, sizeof(host_buf));
    trimmed_ota.toCharArray(ota_buf, sizeof(ota_buf));
    snprintf(sleep_buf, sizeof(sleep_buf), "%d", current_sleep_s);

    WiFiManager wm;
    apply_portal_theme(wm);

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
    WiFiManagerParameter sleep_param("sleep_s",
                                     "Screen sleep timeout (seconds, 0 = never)",
                                     sleep_buf, sizeof(sleep_buf),
                                     "type='number' min='0' max='3600'");
    WiFiManagerParameter reset_touch_param("reset_touch",
                                           "Reset touch calibration",
                                           "1", 2,
                                           "type='checkbox'",
                                           WFM_LABEL_AFTER);

    wm.setAPCallback([](WiFiManager* portal) {
        draw_boot_setup(portal->getConfigPortalSSID().c_str());
    });
    wm.addParameter(&os_host_param);
    wm.addParameter(&os_pass_param);
    wm.addParameter(&ota_pass_param);
    // Labelled text/number fields must precede the label-after checkboxes:
    // WiFiManager renders a checkbox's label inline with no trailing break, so
    // a following field's label would butt up against it. Grouping the
    // checkboxes last keeps each on its own line.
    wm.addParameter(&sleep_param);
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
                               os_host_param, os_pass_param, ota_pass_param,
                               dev_log_param, sleep_param);

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
                                 int current_sleep_s,
                                 bool* touch_cal_reset_out) {
    char host_buf[65] = {};
    char ota_buf[65] = {};
    char sleep_buf[8] = {};
    String normalized_host = osp::normalize_os_host(current_host.c_str()).c_str();
    String trimmed_ota = osp::trim_ascii(current_ota_pass.c_str()).c_str();
    normalized_host.toCharArray(host_buf, sizeof(host_buf));
    trimmed_ota.toCharArray(ota_buf, sizeof(ota_buf));
    snprintf(sleep_buf, sizeof(sleep_buf), "%d", current_sleep_s);

    WiFiManager wm;
    apply_portal_theme(wm);

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
    WiFiManagerParameter sleep_param("sleep_s",
                                     "Screen sleep timeout (seconds, 0 = never)",
                                     sleep_buf, sizeof(sleep_buf),
                                     "type='number' min='0' max='3600'");
    WiFiManagerParameter reset_touch_param("reset_touch",
                                                   "Reset touch calibration",
                                                   "1", 2,
                                                   "type='checkbox'",
                                                   WFM_LABEL_AFTER);

    wm.addParameter(&os_host_param);
    wm.addParameter(&os_pass_param);
    wm.addParameter(&ota_pass_param);
    // Labelled fields before the label-after checkboxes (see AP portal note).
    wm.addParameter(&sleep_param);
    wm.addParameter(&dev_log_param);
    wm.addParameter(&reset_touch_param);

    bool params_saved = false;
    wm.setSaveParamsCallback([&params_saved]() { params_saved = true; });

    wm.startWebPortal();

    String ip_url = "http://";
    ip_url += WiFi.localIP().toString();
    Serial.printf("STA web portal started at %s\n", ip_url.c_str());
    draw_boot_configure(ip_url.c_str());

    const unsigned long t0 = millis();
    const unsigned long timeout_ms = (unsigned long)PORTAL_EDIT_TIMEOUT_S * 1000UL;
    while ((millis() - t0) < timeout_ms) {
        wm.process();
        if (params_saved) break;
        // "Exit" in the portal calls handleExit() which sets abort=true; on the
        // next process() WiFiManager clears webPortalActive. Detect that and
        // break so the caller reboots into normal mode (Exit-doesn't-reboot fix).
        if (!wm.getWebPortalActive()) {
            Serial.println("STA edit portal exited by user");
            break;
        }
        delay(10);
    }
    wm.stopWebPortal();

    if (!params_saved) {
        Serial.println("STA edit portal closed without save");
        return false;
    }

    // WiFi creds unchanged — keep the existing NVS ssid/pass.
    save_portal_params_to_nvs(current_ssid, current_pass, current_host, current_pw_md5,
                               os_host_param, os_pass_param, ota_pass_param,
                               dev_log_param, sleep_param);

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
    int sleep_s = DEFAULT_SLEEP_S;
    load_config_from_nvs(&ssid, &pass, &g_os_host, &g_pw_md5, &ota_pass,
                         &ignored_run_time, &dev_log, nullptr, &sleep_s);

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
        sleep_s = DEFAULT_SLEEP_S;
        Serial.println("Factory clear complete; starting provisioning portal");
        start_provisioning_portal(ssid, pass, g_os_host, g_pw_md5, ota_pass,
                                   dev_log, sleep_s, false, nullptr);
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
                                                     dev_log, sleep_s, &touch_cal_reset);
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
                                                      dev_log, sleep_s, true, &touch_cal_reset);
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
                                   dev_log, sleep_s, false, nullptr);
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
    draw_boot_notice("Calibration saved");
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

    // Bench screen capture: tee this strip to the log client as a binary frame.
    // We are on the UI task and the net task's log tee is suppressed, so this is
    // the sole writer for the frame. Full-screen invalidate renders top-to-bottom
    // full-width bands; the capture completes when the bottom row is reached.
    if (g_capture_active && g_log_client && g_log_client.connected()) {
        char hdr[40];
        const int n = snprintf(hdr, sizeof(hdr), "\x02STRIP %ld %ld %ld %ld\n",
                               static_cast<long>(area->x1),
                               static_cast<long>(area->y1),
                               static_cast<long>(w), static_cast<long>(h));
        capture_write_all(reinterpret_cast<const uint8_t*>(hdr), n);
        capture_write_all(px_map, static_cast<size_t>(w) * h * 2);
    }

    lv_display_flush_ready(disp);
}

static void touchpad_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    uint16_t tx = 0, ty = 0;
    bool pressed;

    // Bench synthetic touch takes priority over the physical panel: drain one
    // queued event per poll so a press then a release land on consecutive reads
    // (LVGL needs both edges to register a click).
    InjTouch inj;
    const bool injected = inj_pop(&inj);
    if (injected) {
        tx = static_cast<uint16_t>(inj.x);
        ty = static_cast<uint16_t>(inj.y);
        pressed = inj.pressed;
    } else {
        pressed = tft.getTouch(&tx, &ty);
    }

    if (!pressed) {
        g_consume_touch_until_release = false;
        g_touch_was_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Trace once per press (rising edge) so a bench can correlate real taps
    // with idle_ms and spot any phantom presses, without spamming every poll.
    if (!g_touch_was_pressed && g_dev_log_enabled) {
        uint32_t idle_ms = 0;
        {
            StateLock lock;
            if (lock && g_ps) idle_ms = g_ps->idle_elapsed_ms();
        }
        Serial.printf("[%s] x=%u y=%u z=%u idle_ms=%lu\n",
                      injected ? "INJ" : "TOUCH",
                      static_cast<unsigned int>(tx),
                      static_cast<unsigned int>(ty),
                      injected ? 0u : static_cast<unsigned int>(tft.getTouchRawZ()),
                      static_cast<unsigned long>(idle_ms));
    }
    g_touch_was_pressed = true;

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
static lv_obj_t* lbl_drop       = nullptr;
static lv_obj_t* lbl_name       = nullptr;
static lv_obj_t* lbl_status     = nullptr;
static lv_obj_t* top_accent     = nullptr;

struct CurrentSlot {
    lv_obj_t* box  = nullptr;
    lv_obj_t* val  = nullptr;
    lv_obj_t* unit = nullptr;
};
static CurrentSlot current_slot;

// Signal-meter widget: 4 ascending bar rectangles (Part 3).
struct SigMeter {
    lv_obj_t* bars[4] = {};
};
static SigMeter sig_panel;
static SigMeter sig_ctrl;

// Battery gauge widget: a battery pictogram (outline + terminal nub + inner
// fill scaled by state-of-charge) followed by a "NN%" label. Sits to the right
// of the PANEL/CTRL meters in the same top-bar group.
struct BattGlyph {
    lv_obj_t* body = nullptr;  // outline; border colour = tier
    lv_obj_t* nub  = nullptr;  // terminal nub; colour = tier
    lv_obj_t* fill = nullptr;  // inner fill; width = %, colour = tier
    lv_obj_t* pct  = nullptr;  // "NN%" label
};
static BattGlyph batt_glyph;

static void set_drop_text_opa(void* obj, int32_t value) {
    lv_obj_set_style_text_opa(static_cast<lv_obj_t*>(obj),
                              static_cast<lv_opa_t>(value), 0);
}

static void update_drop_pulse(bool syncing) {
    static bool pulsing = false;
    if (syncing == pulsing) return;

    if (syncing) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, lbl_drop);
        lv_anim_set_exec_cb(&anim, set_drop_text_opa);
        lv_anim_set_values(&anim, LV_OPA_COVER, 90);
        lv_anim_set_duration(&anim, 600);
        lv_anim_set_playback_duration(&anim, 600);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_start(&anim);
    } else {
        lv_anim_delete(lbl_drop, set_drop_text_opa);
        lv_obj_set_style_text_opa(lbl_drop, LV_OPA_COVER, 0);
    }
    pulsing = syncing;
}

// Smoothed LiPo monitor fed from PIN_BAT_ADC (pure logic in lib/battery_monitor).
static osp::BatteryMonitor g_batt;

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

// M9: Right panel (needs show/hide for programs list and program queue views)
static lv_obj_t* pnl_right       = nullptr;

// M9: Programs list panel (full-width overlay at CONTENT_Y)
static lv_obj_t* pnl_programs    = nullptr;
static lv_obj_t* btn_programs    = nullptr;  // entry button on idle panel
static constexpr int MAX_PROG_ROWS  = 4;
static constexpr int MAX_PROG_PAGES = 6;
static lv_obj_t* prog_rows[MAX_PROG_ROWS]          = {};
static lv_obj_t* prog_row_icon[MAX_PROG_ROWS]      = {};
static lv_obj_t* prog_row_name[MAX_PROG_ROWS]      = {};
static lv_obj_t* prog_row_next[MAX_PROG_ROWS]      = {};
static lv_obj_t* prog_row_btn_toggle[MAX_PROG_ROWS] = {};
static lv_obj_t* prog_row_btn_run[MAX_PROG_ROWS]   = {};
static lv_obj_t* prog_page_dots[MAX_PROG_PAGES]    = {};
static lv_obj_t* prog_page_prev                    = nullptr;  // ‹ pager arrow
static lv_obj_t* prog_page_next                    = nullptr;  // › pager arrow

// M9: Program-run queue view panel
static lv_obj_t* pnl_prog_queue  = nullptr;
static lv_obj_t* lbl_prog_eyebrow = nullptr;
static lv_obj_t* lbl_prog_name   = nullptr;
static lv_obj_t* lbl_prog_cd     = nullptr;

// M9: Program queue action buttons (at ACTION_Y, same row as btn_advance/btn_stop)
static lv_obj_t* btn_prog_adv    = nullptr;
static lv_obj_t* btn_prog_pause  = nullptr;
static lv_obj_t* lbl_prog_pause_txt = nullptr;
static lv_obj_t* btn_prog_stop   = nullptr;

// M9: "Resumes in M:SS" line shown on the queue view while paused.
static lv_obj_t* lbl_prog_resume = nullptr;

// M9: Right-side program queue LIST (station rows) shown during a program run.
static lv_obj_t* pnl_prog_qlist   = nullptr;
static lv_obj_t* lbl_qlist_hdr    = nullptr;  // program name header
static lv_obj_t* lbl_qlist_total  = nullptr;  // total time remaining
static lv_obj_t* qfade_top        = nullptr;  // top fade mask ("more above")
static lv_obj_t* qfade_bottom     = nullptr;  // bottom fade mask ("more below")
static constexpr int MAX_QROWS = 9;
static lv_obj_t* qrow_mark[MAX_QROWS] = {};
static lv_obj_t* qrow_name[MAX_QROWS] = {};
static lv_obj_t* qrow_dur[MAX_QROWS]  = {};

// M9: signal that /jp should be re-fetched after a SetProgramEnabled delivery
static volatile bool g_jp_needs_refresh = false;

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

// Store/retrieve a program pid (int) as lv_obj user_data — for program rows.
static void set_prog_pid(lv_obj_t* obj, int pid) {
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(static_cast<intptr_t>(pid)));
}
static int get_prog_pid(lv_obj_t* obj) {
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

// M9: Programs list navigation
static void ev_open_programs(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->open_programs_list();
}
static void ev_close_programs(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->close_programs_list();
}
static void ev_prog_page_prev(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (!(lock && g_ps)) return;
    g_ps->set_prog_list_page(g_ps->view().prog_list_page - 1);  // clamped in setter
}
static void ev_prog_page_next(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (!(lock && g_ps)) return;
    g_ps->set_prog_list_page(g_ps->view().prog_list_page + 1);  // clamped in setter
}
static void ev_prog_toggle_enabled(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (!(lock && g_ps)) return;
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int pid = get_prog_pid(btn);
    if (pid < 1) return;
    const auto& progs = g_ps->program_list().programs;
    const int idx = pid - 1;
    if (idx >= static_cast<int>(progs.size())) return;
    // Intents take a 0-based program index (/cp?pid= is 0-based in the API).
    g_ps->toggle_program_enabled_intent(idx, !progs[idx].enabled);
}
static void ev_prog_run(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (!(lock && g_ps)) return;
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int pid = get_prog_pid(btn);
    // Intents take a 0-based program index (/mp?pid= is 0-based in the API).
    if (pid >= 1) g_ps->run_program_intent(pid - 1);
}

// M9: Program queue actions
static void ev_prog_pause(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->pause_toggle_intent();
}
static void ev_prog_advance(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->program_advance_intent();
}
static void ev_prog_stop(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->stop();
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

// Full-height layout, used when the station grid is hidden (programs list and
// program-run screens). These surfaces have their own widgets, so they simply
// build at the taller geometry — no runtime resize needed.
static constexpr int FULL_BOTTOM   = SCREEN_H - 4;                  // 316
// Lift the program-run action row off the very bottom edge so Next/Pause/Stop
// are easier to reach. Still sits lower than the manual-run row (ACTION_Y=156)
// — a modest raise, not a full match.
static constexpr int PROG_ACTION_LIFT = 40;
static constexpr int PROG_ACTION_Y = FULL_BOTTOM - ACTION_H - PROG_ACTION_LIFT; // 224
static constexpr int FULL_PANEL_H  = PROG_ACTION_Y - CONTENT_Y - 6; // left panel above the action row
static constexpr int FULL_RIGHT_H  = FULL_BOTTOM - CONTENT_Y;       // right queue list to the bottom
static constexpr int PROG_LIST_H   = FULL_BOTTOM - CONTENT_Y;       // programs-list overlay

// Shared navigation-button height for visual continuity across the
// programs-list "Back" button and the pager ‹ › arrows. (The "Programs" entry
// button on the home screen instead matches the run-time stepper height.)
static constexpr int NAV_BTN_H = 36;
// Programs-list pager dot indicators (shared by build + per-frame re-centring).
static constexpr int PROG_DOT_W = 10, PROG_DOT_H = 10, PROG_DOT_GAP = 8;
static constexpr int PROG_ARROW_W = 46;  // pager arrow button width


// ---------------------------------------------------------------------------
// Signal-meter helpers (Part 3)
// ---------------------------------------------------------------------------

static CurrentSlot build_current_slot(lv_obj_t* parent) {
    CurrentSlot slot;
    slot.box = lv_obj_create(parent);
    lv_obj_remove_style_all(slot.box);
    lv_obj_set_style_bg_opa(slot.box, LV_OPA_TRANSP, 0);
    lv_obj_set_size(slot.box, 46, TOP_H);
    lv_obj_set_style_pad_column(slot.box, 3, 0);
    lv_obj_clear_flag(slot.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(slot.box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(slot.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slot.box, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    slot.val = lv_label_create(slot.box);
    lv_obj_set_width(slot.val, 27);
    lv_label_set_long_mode(slot.val, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(slot.val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(slot.val, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(slot.val, hex_color(CLR_MUTED), 0);
    lv_label_set_text(slot.val, "0");

    slot.unit = lv_label_create(slot.box);
    lv_obj_set_width(slot.unit, 16);
    lv_label_set_long_mode(slot.unit, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(slot.unit, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(slot.unit, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_letter_space(slot.unit, -1, 0);
    lv_obj_set_style_text_color(slot.unit, hex_color(CLR_MUTED), 0);
    lv_label_set_text(slot.unit, "mA");

    return slot;
}

// Build a compact drawn RSSI meter into `parent` (a pre-created flex-row group).
// Layout: "P"/"C" label followed by 4 ascending bar rectangles.
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

    // "P" / "C" text label.
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
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

// Battery pictogram geometry. Body is 18×11 with a 1 px border + 1 px pad, so
// the inner fill spans BATT_FILL_MAX_W × 7 px. A 2 px nub hangs off the right.
static constexpr int BATT_BODY_W    = 18;
static constexpr int BATT_BODY_H    = 11;
static constexpr int BATT_FILL_MAX_W = BATT_BODY_W - 2 /*border*/ - 2 /*pad*/;  // 14

// Build the battery gauge (pictogram + percent) into `parent`.
static BattGlyph build_batt_glyph(lv_obj_t* parent) {
    BattGlyph g;

    // Flex-row: pictogram + percent label, small gap.
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_size(row, LV_SIZE_CONTENT, TOP_H);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Pictogram holder (body + nub in absolute layout).
    lv_obj_t* pic = lv_obj_create(row);
    lv_obj_remove_style_all(pic);
    lv_obj_set_style_bg_opa(pic, LV_OPA_TRANSP, 0);
    lv_obj_set_size(pic, BATT_BODY_W + 2, BATT_BODY_H);  // +2 for the nub
    lv_obj_clear_flag(pic, LV_OBJ_FLAG_SCROLLABLE);

    // Body outline.
    g.body = lv_obj_create(pic);
    lv_obj_remove_style_all(g.body);
    lv_obj_set_size(g.body, BATT_BODY_W, BATT_BODY_H);
    lv_obj_set_pos(g.body, 0, 0);
    lv_obj_set_style_bg_opa(g.body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g.body, 1, 0);
    lv_obj_set_style_border_color(g.body, hex_color(CLR_TEAL), 0);
    lv_obj_set_style_radius(g.body, 2, 0);
    lv_obj_set_style_pad_all(g.body, 1, 0);
    lv_obj_clear_flag(g.body, LV_OBJ_FLAG_SCROLLABLE);

    // Terminal nub.
    g.nub = lv_obj_create(pic);
    lv_obj_remove_style_all(g.nub);
    lv_obj_set_size(g.nub, 2, 5);
    lv_obj_set_pos(g.nub, BATT_BODY_W, (BATT_BODY_H - 5) / 2);
    lv_obj_set_style_bg_color(g.nub, hex_color(CLR_TEAL), 0);
    lv_obj_set_style_bg_opa(g.nub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g.nub, 1, 0);
    lv_obj_clear_flag(g.nub, LV_OBJ_FLAG_SCROLLABLE);

    // Inner fill (left-anchored, width scaled by %).
    g.fill = lv_obj_create(g.body);
    lv_obj_remove_style_all(g.fill);
    lv_obj_set_size(g.fill, BATT_FILL_MAX_W, 7);
    lv_obj_set_pos(g.fill, 0, 0);
    lv_obj_set_style_bg_color(g.fill, hex_color(CLR_TEAL), 0);
    lv_obj_set_style_bg_opa(g.fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g.fill, 1, 0);
    lv_obj_clear_flag(g.fill, LV_OBJ_FLAG_SCROLLABLE);

    // Percent label.
    g.pct = lv_label_create(row);
    lv_obj_set_width(g.pct, 30);
    lv_obj_set_style_text_align(g.pct, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(g.pct, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g.pct, hex_color(CLR_LEDE), 0);
    lv_label_set_text(g.pct, "--%");

    return g;
}

// Update fill width, tier colour and percent text.
static void update_batt_glyph(const BattGlyph& g, int percent,
                              osp::BatteryTier tier) {
    if (!g.body) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const uint32_t clr = (tier == osp::BatteryTier::Healthy) ? CLR_TEAL
                       : (tier == osp::BatteryTier::Low)     ? CLR_AMBER
                                                             : CLR_RED;
    lv_obj_set_style_border_color(g.body, hex_color(clr), 0);
    lv_obj_set_style_bg_color(g.nub, hex_color(clr), 0);
    lv_obj_set_style_bg_color(g.fill, hex_color(clr), 0);
    lv_obj_set_style_text_color(
        g.pct, hex_color(tier == osp::BatteryTier::Healthy ? CLR_LEDE : clr), 0);

    // Fill width tracks %, but keep a visible sliver whenever a cell is present.
    int w = (BATT_FILL_MAX_W * percent + 50) / 100;
    if (w < 2) w = 2;
    if (w > BATT_FILL_MAX_W) w = BATT_FILL_MAX_W;
    lv_obj_set_width(g.fill, w);

    char b[8];
    snprintf(b, sizeof(b), "%d%%", percent);
    lv_label_set_text(g.pct, b);
}

// Sample the battery ADC (multisampled) on a slow cadence and feed the monitor.
static void poll_battery() {
    static uint32_t last_ms = 0;
    const uint32_t now = millis();
    if (last_ms != 0 && (now - last_ms) < BAT_SAMPLE_MS) return;
    last_ms = now;
    uint32_t sum = 0;
    for (int i = 0; i < BAT_ADC_SAMPLES; ++i) {
        sum += analogReadMilliVolts(PIN_BAT_ADC);
    }
    g_batt.add_tap_sample(static_cast<int>(sum / BAT_ADC_SAMPLES));
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

        lv_obj_t* left_group = lv_obj_create(bar);
        lv_obj_remove_style_all(left_group);
        lv_obj_set_style_bg_opa(left_group, LV_OPA_TRANSP, 0);
        lv_obj_set_size(left_group, 220, TOP_H);
        lv_obj_set_style_pad_column(left_group, 6, 0);
        lv_obj_clear_flag(left_group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(left_group, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(left_group, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_group, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(left_group, LV_ALIGN_LEFT_MID, 4, 0);

        lbl_drop = lv_label_create(left_group);
        lv_obj_set_style_text_font(lbl_drop, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_drop, hex_color(CLR_TEAL), 0);
        lv_label_set_text(lbl_drop, LV_SYMBOL_TINT);

        lbl_name = lv_label_create(left_group);
        lv_obj_set_size(lbl_name, 1, 14);
        lv_obj_set_flex_grow(lbl_name, 1);
        lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_name, hex_color(CLR_TEXT), 0);
        lv_label_set_text(lbl_name, "controller");

        lbl_status = lv_label_create(left_group);
        lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(lbl_status, 1, 0);
        lv_obj_set_style_text_color(lbl_status, hex_color(CLR_TEAL), 0);
        lv_label_set_text(lbl_status, "");

        // Fixed right cluster: current, divider, panel/controller signal, battery.
        lv_obj_t* sig_group = lv_obj_create(bar);
        lv_obj_remove_style_all(sig_group);
        lv_obj_set_style_bg_opa(sig_group, LV_OPA_TRANSP, 0);
        lv_obj_set_size(sig_group, LV_SIZE_CONTENT, TOP_H);
        lv_obj_set_style_pad_column(sig_group, 12, 0);
        lv_obj_clear_flag(sig_group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(sig_group, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(sig_group, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sig_group, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(sig_group, LV_ALIGN_RIGHT_MID, -4, 0);

        current_slot = build_current_slot(sig_group);
        obj_set_hidden(current_slot.box, true);

        lv_obj_t* divider = lv_obj_create(sig_group);
        lv_obj_remove_style_all(divider);
        lv_obj_set_size(divider, 2, 16);
        lv_obj_set_style_bg_color(divider, hex_color(CLR_TEALDIM), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(divider, 1, 0);
        lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

        sig_panel = build_sig_meter(sig_group, "P");
        sig_ctrl  = build_sig_meter(sig_group, "C");
        batt_glyph = build_batt_glyph(sig_group);

        top_accent = lv_obj_create(scr);
        lv_obj_remove_style_all(top_accent);
        lv_obj_set_size(top_accent, SCREEN_W, 3);
        lv_obj_set_pos(top_accent, 0, TOP_H - 3);
        lv_obj_set_style_bg_color(top_accent, hex_color(CLR_TEALDIM), 0);
        lv_obj_set_style_bg_opa(top_accent, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(top_accent, 0, 0);
        lv_obj_clear_flag(top_accent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_foreground(top_accent);
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

    // NOTE: the "Programs" entry button lives in the right settings panel,
    // below Auto-advance (built further down), to match the mockup.

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
    // Constrain to a single line so LONG_DOT ellipsizes instead of wrapping —
    // otherwise a long name grows down into the countdown ticker at y=66.
    // montserrat_24 line height is ~29px; 30 keeps one line with a small margin.
    lv_obj_set_height(lbl_stn_name, 30);
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

    // ---- Left panel: Program queue view (M9) ---------------------------
    // The station grid is hidden during a program run, so this panel uses the
    // full height down to the action row.
    pnl_prog_queue = lv_obj_create(scr);
    lv_obj_set_size(pnl_prog_queue, LEFT_W, FULL_PANEL_H);
    lv_obj_set_pos(pnl_prog_queue, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(pnl_prog_queue, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(pnl_prog_queue, 0, 0);
    lv_obj_set_style_pad_all(pnl_prog_queue, 14, 0);
    lv_obj_clear_flag(pnl_prog_queue, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pnl_prog_queue, LV_OBJ_FLAG_HIDDEN);

    lbl_prog_eyebrow = lv_label_create(pnl_prog_queue);
    lv_label_set_text(lbl_prog_eyebrow, "STATION");
    lv_obj_set_style_text_font(lbl_prog_eyebrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_prog_eyebrow, hex_color(CLR_TEAL), 0);
    lv_obj_align(lbl_prog_eyebrow, LV_ALIGN_TOP_LEFT, 0, 6);

    // Current station name (repurposed: the running zone, not the program name).
    // The program name is shown only above the queue (right panel) per the
    // mockup — the status column mirrors the manual-run layout.
    lbl_prog_name = lv_label_create(pnl_prog_queue);
    lv_label_set_text(lbl_prog_name, "");
    lv_obj_set_width(lbl_prog_name, LEFT_W - 28);
    // One-line height so a long current-station name ellipsizes instead of
    // wrapping down into the big program countdown (lbl_prog_cd at y=70).
    // montserrat_24 line height ~29px; name band y28–58 clears the timer.
    lv_obj_set_height(lbl_prog_name, 30);
    lv_label_set_long_mode(lbl_prog_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_prog_name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_prog_name, hex_color(CLR_TEXT), 0);
    lv_obj_align(lbl_prog_name, LV_ALIGN_TOP_LEFT, 0, 28);

    lbl_prog_cd = lv_label_create(pnl_prog_queue);
    lv_label_set_text(lbl_prog_cd, "0:00");
    lv_obj_set_style_text_font(lbl_prog_cd, &ui_font_countdown_48, 0);
    lv_obj_set_style_text_color(lbl_prog_cd, hex_color(CLR_AMBER), 0);
    lv_obj_align(lbl_prog_cd, LV_ALIGN_TOP_LEFT, 0, 70);

    // "Resumes in M:SS" — a second line below the countdown while paused.
    lbl_prog_resume = lv_label_create(pnl_prog_queue);
    lv_label_set_text(lbl_prog_resume, "");
    lv_obj_set_style_text_font(lbl_prog_resume, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_prog_resume, hex_color(CLR_AMBER), 0);
    lv_obj_add_flag(lbl_prog_resume, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(lbl_prog_resume, LV_ALIGN_TOP_LEFT, 0, 132);

    // ---- Action row (Advance / Stop) -----------------------------------
    static constexpr int ACTION_SIDE_PAD = 10;
    static constexpr int ACTION_GAP = 10;
    const int action_btn_w = (LEFT_W - (2 * ACTION_SIDE_PAD) - ACTION_GAP) / 2;

    btn_advance = make_btn(scr, "Next " LV_SYMBOL_RIGHT,
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

    // ---- Program queue action row (M9): Advance / Pause / Stop ---------
    // Anchored at the full-height action row (the grid is hidden during runs).
    {
        static constexpr int PROG_SIDE_PAD = 8;
        static constexpr int PROG_BTN_GAP  = 8;
        const int prog_btn_w = (LEFT_W - (2 * PROG_SIDE_PAD) - (2 * PROG_BTN_GAP)) / 3;

        btn_prog_adv = make_btn(scr, "Next " LV_SYMBOL_RIGHT,
                                CLR_TEAL, CLR_BG, &lv_font_montserrat_16, 10);
        lv_obj_set_size(btn_prog_adv, prog_btn_w, ACTION_H);
        lv_obj_set_pos(btn_prog_adv, PROG_SIDE_PAD, PROG_ACTION_Y);
        lv_obj_add_flag(btn_prog_adv, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(btn_prog_adv, ev_prog_advance, LV_EVENT_CLICKED, nullptr);

        btn_prog_pause = make_btn(scr, "Pause",
                                  CLR_LINE, CLR_TEXT, &lv_font_montserrat_16, 10);
        lv_obj_set_size(btn_prog_pause, prog_btn_w, ACTION_H);
        lv_obj_set_pos(btn_prog_pause, PROG_SIDE_PAD + prog_btn_w + PROG_BTN_GAP, PROG_ACTION_Y);
        lv_obj_add_flag(btn_prog_pause, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(btn_prog_pause, ev_prog_pause, LV_EVENT_CLICKED, nullptr);
        lbl_prog_pause_txt = static_cast<lv_obj_t*>(lv_obj_get_child(btn_prog_pause, 0));

        btn_prog_stop = make_btn(scr, LV_SYMBOL_STOP " Stop",
                                 CLR_RED, CLR_BG, &lv_font_montserrat_16, 10);
        lv_obj_set_size(btn_prog_stop, prog_btn_w, ACTION_H);
        lv_obj_set_pos(btn_prog_stop, PROG_SIDE_PAD + 2*(prog_btn_w + PROG_BTN_GAP), PROG_ACTION_Y);
        lv_obj_add_flag(btn_prog_stop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(btn_prog_stop, ev_prog_stop, LV_EVENT_CLICKED, nullptr);
    }

    // ---- Right panel ---------------------------------------------------
    {
        pnl_right = lv_obj_create(scr);
        lv_obj_t* pnl = pnl_right;
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

        // Auto-advance row sits directly under the stepper (no divider) so the
        // two run-time controls read as a group. ~8 px gap keeps touch targets
        // from colliding.
        static constexpr int AA_Y = STEP_Y + STEP_H + 8;  // just below stepper
        static constexpr int AA_H = 38;
        lv_obj_t* row_auto_adv = lv_obj_create(pnl);
        lv_obj_set_size(row_auto_adv, PANEL_CONTENT_W, AA_H);
        lv_obj_align(row_auto_adv, LV_ALIGN_TOP_LEFT, 0, AA_Y);
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

        // "Programs" entry button — vertically centred in the space between the
        // Auto-advance row and the panel's bottom edge (biased a few px lower so
        // it reads as centred in the column rather than hugging Auto-advance).
        // Height matches the run-time stepper (STEP_H) so the right column reads
        // as one family of controls. A leading list glyph reads as "a list of
        // programs" (the screen it opens); text uses CLR_TEXT like the other
        // secondary buttons (-, +, Pause) rather than a teal accent.
        static constexpr int PROG_BTN_H = STEP_H;
        static constexpr int PANEL_USABLE_H = CONTENT_H - (2 * PANEL_PAD);
        const int aa_bottom = AA_Y + AA_H;
        int prog_btn_y = aa_bottom + ((PANEL_USABLE_H - aa_bottom) - PROG_BTN_H) / 2 + 6;
        if (prog_btn_y > PANEL_USABLE_H - PROG_BTN_H) {
            prog_btn_y = PANEL_USABLE_H - PROG_BTN_H;
        }
        btn_programs = make_btn(pnl, LV_SYMBOL_LIST " Programs",
                                CLR_LINE, CLR_TEXT, &lv_font_montserrat_16, 8);
        lv_obj_set_size(btn_programs, PANEL_CONTENT_W, PROG_BTN_H);
        lv_obj_align(btn_programs, LV_ALIGN_TOP_LEFT, 0, prog_btn_y);
        lv_obj_add_event_cb(btn_programs, ev_open_programs, LV_EVENT_CLICKED, nullptr);
    }

    // ---- Right-side program QUEUE list (M9) ----------------------------
    // Shown during a program run in place of the settings panel (full height,
    // since the grid is hidden). Lists the program's stations (done / current /
    // upcoming) reconstructed from the program definition + live /jc ps[].
    {
        pnl_prog_qlist = lv_obj_create(scr);
        lv_obj_set_size(pnl_prog_qlist, RIGHT_W, FULL_RIGHT_H);
        lv_obj_set_pos(pnl_prog_qlist, LEFT_W, CONTENT_Y);
        lv_obj_set_style_bg_color(pnl_prog_qlist, hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(pnl_prog_qlist, 0, 0);
        lv_obj_set_style_pad_all(pnl_prog_qlist, 10, 0);
        lv_obj_clear_flag(pnl_prog_qlist, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pnl_prog_qlist, LV_OBJ_FLAG_HIDDEN);

        const int qcontent_w = RIGHT_W - 20;

        lbl_qlist_hdr = lv_label_create(pnl_prog_qlist);
        lv_label_set_text(lbl_qlist_hdr, "");
        lv_obj_set_width(lbl_qlist_hdr, qcontent_w);
        // One-line height (montserrat_16 ~19px) so a long program name
        // ellipsizes instead of wrapping into the total-time line at y=22.
        lv_obj_set_height(lbl_qlist_hdr, 20);
        lv_label_set_long_mode(lbl_qlist_hdr, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(lbl_qlist_hdr, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_qlist_hdr, hex_color(CLR_TEXT), 0);
        lv_obj_align(lbl_qlist_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        lbl_qlist_total = lv_label_create(pnl_prog_qlist);
        lv_label_set_text(lbl_qlist_total, "");
        lv_obj_set_width(lbl_qlist_total, qcontent_w);
        lv_label_set_long_mode(lbl_qlist_total, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(lbl_qlist_total, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_qlist_total, hex_color(CLR_MUTED), 0);
        lv_obj_align(lbl_qlist_total, LV_ALIGN_TOP_LEFT, 0, 22);

        static constexpr int QROW_Y0 = 50;
        static constexpr int QROW_H  = 24;

        for (int i = 0; i < MAX_QROWS; ++i) {
            const int y = QROW_Y0 + i * QROW_H;

            qrow_mark[i] = lv_label_create(pnl_prog_qlist);
            lv_label_set_text(qrow_mark[i], "");
            lv_obj_set_style_text_font(qrow_mark[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(qrow_mark[i], hex_color(CLR_MUTED), 0);
            lv_obj_set_pos(qrow_mark[i], 0, y);

            qrow_name[i] = lv_label_create(pnl_prog_qlist);
            lv_label_set_text(qrow_name[i], "");
            lv_obj_set_width(qrow_name[i], qcontent_w - 22 - 48);
            // Constrain to a single line so LONG_DOT truncates with an ellipsis
            // instead of wrapping (a long station name would otherwise grow
            // vertically and collide into the next queue row).
            lv_obj_set_height(qrow_name[i], 18);
            lv_label_set_long_mode(qrow_name[i], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(qrow_name[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(qrow_name[i], hex_color(CLR_TEXT), 0);
            lv_obj_set_pos(qrow_name[i], 22, y);

            qrow_dur[i] = lv_label_create(pnl_prog_qlist);
            lv_label_set_text(qrow_dur[i], "");
            lv_obj_set_width(qrow_dur[i], 44);
            lv_obj_set_style_text_align(qrow_dur[i], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_font(qrow_dur[i], &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(qrow_dur[i], hex_color(CLR_MUTED), 0);
            lv_obj_align(qrow_dur[i], LV_ALIGN_TOP_RIGHT, 0, y);
        }

        // Fade masks (created last so they render on top of the rows). A short
        // vertical gradient of the panel background, opaque at the list edge
        // fading to transparent, signals there are more rows above/below — the
        // LVGL equivalent of the mockup's CSS mask. Toggled per frame.
        const int qfade_h = 16;
        qfade_top = lv_obj_create(pnl_prog_qlist);
        lv_obj_remove_style_all(qfade_top);
        lv_obj_set_size(qfade_top, qcontent_w, qfade_h);
        lv_obj_set_pos(qfade_top, 0, QROW_Y0 - 3);
        lv_obj_set_style_bg_color(qfade_top, hex_color(CLR_BG), 0);
        lv_obj_set_style_bg_grad_color(qfade_top, hex_color(CLR_BG), 0);
        lv_obj_set_style_bg_grad_dir(qfade_top, LV_GRAD_DIR_VER, 0);
        // Base fill must be opaque; the per-stop main/grad opacities below create
        // the actual fade. Without this, remove_style_all leaves bg_opa=0 and the
        // gradient never renders (the "no fade mask" bug).
        lv_obj_set_style_bg_opa(qfade_top, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(qfade_top, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_opa(qfade_top, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(qfade_top, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(qfade_top, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(qfade_top, LV_OBJ_FLAG_HIDDEN);

        qfade_bottom = lv_obj_create(pnl_prog_qlist);
        lv_obj_remove_style_all(qfade_bottom);
        lv_obj_set_size(qfade_bottom, qcontent_w, qfade_h);
        lv_obj_set_pos(qfade_bottom, 0, QROW_Y0 + MAX_QROWS * QROW_H - qfade_h - 2);
        lv_obj_set_style_bg_color(qfade_bottom, hex_color(CLR_BG), 0);
        lv_obj_set_style_bg_grad_color(qfade_bottom, hex_color(CLR_BG), 0);
        lv_obj_set_style_bg_grad_dir(qfade_bottom, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(qfade_bottom, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(qfade_bottom, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_opa(qfade_bottom, LV_OPA_COVER, 0);
        lv_obj_clear_flag(qfade_bottom, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(qfade_bottom, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(qfade_bottom, LV_OBJ_FLAG_HIDDEN);
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

    // ---- Programs list panel (M9) — full-width overlay -----------------
    // The station grid is hidden here, so the panel uses the full height.
    //   Header: PROG_HDR_H px (larger Back target)
    //   4 rows:  PROG_ROW_H px each
    //   Pager:   below the rows
    static constexpr int PROG_HDR_H  = 44;
    static constexpr int PROG_ROW_H  = 48;
    // Pager sits in the empty band below the last row, vertically centred
    // between the bottom of the last program row and the bottom of the panel.
    static constexpr int PROG_ROWS_BOTTOM = PROG_HDR_H + MAX_PROG_ROWS * PROG_ROW_H;
    static constexpr int PROG_PAGER_CY = PROG_ROWS_BOTTOM +
                                         (PROG_LIST_H - PROG_ROWS_BOTTOM) / 2;
    static constexpr int PROG_DOT_Y  = PROG_PAGER_CY - PROG_DOT_H / 2;
    // Right-side button metrics within each row.
    static constexpr int PROG_RUN_W  = 80;   // Run button width
    static constexpr int PROG_TOG_W  = 86;   // Toggle (Enable/Disable) button width
    static constexpr int PROG_BTN_RP = 4;    // right padding from row edge
    static constexpr int PROG_BTN_G  = 4;    // gap between toggle and run
    static constexpr int PROG_BTN_H  = PROG_ROW_H - 12;
    static constexpr int PROG_BTN_Y  = (PROG_ROW_H - PROG_BTN_H) / 2;
    // x position of toggle and run buttons (within row, row width = SCREEN_W)
    static constexpr int PROG_RUN_X  = SCREEN_W - PROG_BTN_RP - PROG_RUN_W;
    static constexpr int PROG_TOG_X  = PROG_RUN_X - PROG_BTN_G - PROG_TOG_W;
    // Left content area: enable/disable icon + name / next-run label width
    static constexpr int PROG_ICON_W = 22;  // enable/disable glyph column
    static constexpr int PROG_TEXT_X = 8 + PROG_ICON_W;
    static constexpr int PROG_NAME_W = PROG_TOG_X - PROG_TEXT_X - 8;  // gap=8

    pnl_programs = lv_obj_create(scr);
    lv_obj_set_size(pnl_programs, SCREEN_W, PROG_LIST_H);
    lv_obj_set_pos(pnl_programs, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(pnl_programs, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(pnl_programs, 0, 0);
    lv_obj_set_style_pad_all(pnl_programs, 0, 0);
    lv_obj_clear_flag(pnl_programs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pnl_programs, LV_OBJ_FLAG_HIDDEN);

    // Header row
    {
        lv_obj_t* hdr = lv_obj_create(pnl_programs);
        lv_obj_set_size(hdr, SCREEN_W, PROG_HDR_H);
        lv_obj_set_pos(hdr, 0, 0);
        lv_obj_set_style_bg_color(hdr, hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(hdr, 0, 0);
        lv_obj_set_style_pad_all(hdr, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lbl_title = lv_label_create(hdr);
        lv_label_set_text(lbl_title, "PROGRAMS");
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_title, hex_color(CLR_TEXT), 0);
        lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t* btn_back = make_btn(hdr, LV_SYMBOL_LEFT " Back",
                                      CLR_LINE, CLR_TEXT,
                                      &lv_font_montserrat_20, 8);
        lv_obj_set_size(btn_back, 128, NAV_BTN_H);
        lv_obj_align(btn_back, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_add_event_cb(btn_back, ev_close_programs, LV_EVENT_CLICKED, nullptr);
    }

    // 4 program rows
    for (int r = 0; r < MAX_PROG_ROWS; ++r) {
        const int ry = PROG_HDR_H + r * PROG_ROW_H;

        prog_rows[r] = lv_obj_create(pnl_programs);
        lv_obj_set_size(prog_rows[r], SCREEN_W, PROG_ROW_H);
        lv_obj_set_pos(prog_rows[r], 0, ry);
        lv_obj_set_style_bg_color(prog_rows[r], hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(prog_rows[r], 0, 0);
        lv_obj_set_style_pad_all(prog_rows[r], 0, 0);
        lv_obj_clear_flag(prog_rows[r], LV_OBJ_FLAG_SCROLLABLE);

        // Enable/disable icon on the name line (left). Glyph + colour convey
        // program state (paired with dimming of the name); no chip/word.
        prog_row_icon[r] = lv_label_create(prog_rows[r]);
        lv_label_set_text(prog_row_icon[r], "");
        lv_obj_set_style_text_font(prog_row_icon[r], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(prog_row_icon[r], hex_color(CLR_TEAL), 0);
        lv_obj_set_pos(prog_row_icon[r], 8, 8);

        // Program name (top line). Colour conveys enabled state (dimmed when
        // disabled) — no separate ENABLED/DISABLED chip.
        prog_row_name[r] = lv_label_create(prog_rows[r]);
        lv_label_set_text(prog_row_name[r], "");
        lv_obj_set_width(prog_row_name[r], PROG_NAME_W);
        // Constrain to a single line so LONG_DOT ellipsizes instead of wrapping
        // — otherwise a long program name grows down into the schedule/meta line
        // at y=28. montserrat_16 line height is ~19px; 20 keeps one line.
        lv_obj_set_height(prog_row_name[r], 20);
        lv_label_set_long_mode(prog_row_name[r], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(prog_row_name[r], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(prog_row_name[r], hex_color(CLR_TEXT), 0);
        lv_obj_set_pos(prog_row_name[r], PROG_TEXT_X, 6);

        // Next-run + zones/runtime meta (bottom line, full width up to buttons).
        prog_row_next[r] = lv_label_create(prog_rows[r]);
        lv_label_set_text(prog_row_next[r], "");
        lv_obj_set_width(prog_row_next[r], PROG_NAME_W);
        lv_label_set_long_mode(prog_row_next[r], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(prog_row_next[r], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(prog_row_next[r], hex_color(CLR_MUTED), 0);
        lv_obj_set_pos(prog_row_next[r], PROG_TEXT_X, 28);

        // Toggle (Enable/Disable) button
        prog_row_btn_toggle[r] = make_btn(prog_rows[r], "Disable",
                                          CLR_LINE, CLR_TEXT,
                                          &lv_font_montserrat_12, 6);
        lv_obj_set_size(prog_row_btn_toggle[r], PROG_TOG_W, PROG_BTN_H);
        lv_obj_set_pos(prog_row_btn_toggle[r], PROG_TOG_X, PROG_BTN_Y);
        lv_obj_add_event_cb(prog_row_btn_toggle[r], ev_prog_toggle_enabled,
                            LV_EVENT_CLICKED, nullptr);

        // Run button
        prog_row_btn_run[r] = make_btn(prog_rows[r], "Run " LV_SYMBOL_RIGHT,
                                       CLR_TEAL, CLR_BG,
                                       &lv_font_montserrat_12, 6);
        lv_obj_set_size(prog_row_btn_run[r], PROG_RUN_W, PROG_BTN_H);
        lv_obj_set_pos(prog_row_btn_run[r], PROG_RUN_X, PROG_BTN_Y);
        lv_obj_add_event_cb(prog_row_btn_run[r], ev_prog_run,
                            LV_EVENT_CLICKED, nullptr);
    }

    // Pager: ‹ › arrow buttons flanking a centred row of page dots (no text).
    {
        // Dots are non-interactive indicators; the arrows drive paging. Their X
        // is re-centred every frame in ui_update() from the live page count, so
        // the build-time X here is just a placeholder — only size/style/Y are
        // set now (dots stay hidden until there is more than one page).
        for (int d = 0; d < MAX_PROG_PAGES; ++d) {
            prog_page_dots[d] = lv_obj_create(pnl_programs);
            lv_obj_remove_style_all(prog_page_dots[d]);
            lv_obj_set_size(prog_page_dots[d], PROG_DOT_W, PROG_DOT_H);
            lv_obj_set_pos(prog_page_dots[d], 0, PROG_DOT_Y);
            lv_obj_set_style_radius(prog_page_dots[d], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(prog_page_dots[d], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(prog_page_dots[d], hex_color(CLR_LINE), 0);
            lv_obj_add_flag(prog_page_dots[d], LV_OBJ_FLAG_HIDDEN);
        }

        // Touch-sized arrow buttons: same height as the Back button (NAV_BTN_H)
        // for nav-button continuity, vertically centred on the pager band.
        static constexpr int ARROW_INSET = 14;
        const int arrow_y = PROG_PAGER_CY - NAV_BTN_H / 2;

        prog_page_prev = make_btn(pnl_programs, LV_SYMBOL_LEFT,
                                  CLR_LINE, CLR_TEXT, &lv_font_montserrat_20, 8);
        lv_obj_set_size(prog_page_prev, PROG_ARROW_W, NAV_BTN_H);
        lv_obj_set_pos(prog_page_prev, ARROW_INSET, arrow_y);
        lv_obj_add_event_cb(prog_page_prev, ev_prog_page_prev,
                            LV_EVENT_CLICKED, nullptr);
        lv_obj_add_flag(prog_page_prev, LV_OBJ_FLAG_HIDDEN);

        prog_page_next = make_btn(pnl_programs, LV_SYMBOL_RIGHT,
                                  CLR_LINE, CLR_TEXT, &lv_font_montserrat_20, 8);
        lv_obj_set_size(prog_page_next, PROG_ARROW_W, NAV_BTN_H);
        lv_obj_set_pos(prog_page_next, SCREEN_W - PROG_ARROW_W - ARROW_INSET, arrow_y);
        lv_obj_add_event_cb(prog_page_next, ev_prog_page_next,
                            LV_EVENT_CLICKED, nullptr);
        lv_obj_add_flag(prog_page_next, LV_OBJ_FLAG_HIDDEN);
    }
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
    char buf[80];

    const bool show_syncing = v.show_syncing;
    const bool running      = (v.phase == osp::Phase::Running);
    const bool prog_running = (v.phase == osp::Phase::ProgramRunning);
    const bool any_running  = running || prog_running;
    const bool show_programs = v.showing_programs_list;

    const osp::TopBarState top_state = osp::resolve_top_bar_state(v);
    const std::string status_text = osp::top_bar_status_text(v);
    uint32_t drop_color = CLR_TEAL;
    uint32_t name_color = CLR_TEXT;
    uint32_t status_color = CLR_TEAL;
    uint32_t rule_color = CLR_TEALDIM;
    switch (top_state) {
        case osp::TopBarState::Syncing:
        case osp::TopBarState::Reconnecting:
            drop_color = CLR_AMBER;
            name_color = CLR_MUTED;
            status_color = CLR_AMBER;
            rule_color = CLR_AMBER;
            break;
        case osp::TopBarState::AuthError:
        case osp::TopBarState::Offline:
            drop_color = CLR_RED;
            name_color = CLR_RED;
            status_color = CLR_RED;
            rule_color = CLR_RED;
            break;
        case osp::TopBarState::Disabled:
            status_color = CLR_RED;
            rule_color = CLR_RED;
            break;
        case osp::TopBarState::RainDelay:
        case osp::TopBarState::Clean:
            break;
    }

    lv_label_set_text(lbl_name, v.controller_identity.c_str());
    lv_label_set_text(lbl_status, status_text.c_str());
    lv_obj_set_style_text_color(lbl_drop, hex_color(drop_color), 0);
    lv_obj_set_style_text_color(lbl_name, hex_color(name_color), 0);
    lv_obj_set_style_text_color(lbl_status, hex_color(status_color), 0);
    lv_obj_set_style_bg_color(top_accent, hex_color(rule_color), 0);
    update_drop_pulse(top_state == osp::TopBarState::Syncing ||
                      top_state == osp::TopBarState::Reconnecting);

    obj_set_hidden(current_slot.box, !v.has_current);
    if (v.has_current) {
        snprintf(buf, sizeof(buf), "%d", v.current_ma);
        lv_label_set_text(current_slot.val, buf);
        lv_obj_set_style_text_color(
            current_slot.val,
            hex_color(v.current_ma > 0 ? CLR_TEXT : CLR_MUTED), 0);
    }

    // Drawn RSSI bar meters (Part 3).
    update_sig_meter(sig_panel, osp::rssi_to_bars(WiFi.RSSI()),
                     WiFi.status() == WL_CONNECTED);
    update_sig_meter(sig_ctrl, osp::rssi_to_bars(v.ctrl_rssi),
                     v.link == osp::LinkState::Connected &&
                         top_state != osp::TopBarState::Syncing);

    // Battery gauge: sample the ADC on a slow cadence, render the smoothed
    // state-of-charge. Always shown — an absent cell floats the sense node high
    // (indistinguishable from full), so there is no reliable "no battery" state.
    poll_battery();
    const int battery_percent =
        g_batt_override_percent >= 0 ? g_batt_override_percent
                                     : g_batt.percent();
    update_batt_glyph(
        batt_glyph, battery_percent,
        osp::battery_tier_from_percent(battery_percent));

    // Phase visibility
    obj_set_hidden(pnl_idle,       any_running || show_programs);
    obj_set_hidden(pnl_running,    !running);
    obj_set_hidden(pnl_prog_queue, !prog_running);
    obj_set_hidden(pnl_prog_qlist, !prog_running);
    obj_set_hidden(pnl_programs,   !show_programs);
    obj_set_hidden(pnl_right,      show_programs || prog_running);
    // The Programs entry button lives in the right panel; hide it while a
    // manual station is running (open_programs_list only works from idle).
    obj_set_hidden(btn_programs,   any_running);
    obj_set_hidden(btn_advance,    !running);
    obj_set_hidden(btn_stop,       !running);
    obj_set_hidden(btn_prog_adv,   !prog_running);
    obj_set_hidden(btn_prog_pause, !prog_running);
    obj_set_hidden(btn_prog_stop,  !prog_running);
    // Station grid is only useful for idle (start a station) and manual runs
    // (jump to a station). Hide it on the programs list and during program runs.
    obj_set_hidden(grid_cont,      show_programs || prog_running);
    obj_set_hidden(lbl_grid_title, show_programs || prog_running);

    if (!any_running && !show_programs) {
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
                            ? stns[v.running_sid].name.c_str() : "Station";
        lv_label_set_text(lbl_stn_name, name);

        // Countdown (MM:SS)
        char cd[16];
        fmt_countdown(cd, v.countdown_s);
        lv_label_set_text(lbl_countdown, cd);
    }

    // M9: Program queue view
    if (prog_running) {
        const auto& pr = v.prog_run;
        const auto& progs = g_ps->program_list().programs;
        const auto& stns = g_model.stations();

        // Program name — shown only as the queue-list header (right panel),
        // not on the status column (matches the mockup).
        const char* prog_name = "Station Queue";  // generic header until a program is identified
        if (pr.program_index >= 0 &&
                pr.program_index < static_cast<int>(progs.size())) {
            prog_name = progs[pr.program_index].name.c_str();
        }

        // Eyebrow: identified program -> "STATION N OF M"; unidentified/external
        // run -> honest "N STATIONS LEFT" (we do not know the full set/position).
        const std::string eyebrow = osp::program_run_eyebrow(
            pr.program_index, pr.current_station_number, pr.station_count);
        lv_label_set_text(lbl_prog_eyebrow, eyebrow.c_str());

        // Headline: current station name.
        if (pr.current_sid >= 0 &&
                pr.current_sid < static_cast<int>(stns.size())) {
            lv_label_set_text(lbl_prog_name, stns[pr.current_sid].name.c_str());
        } else {
            lv_label_set_text(lbl_prog_name, "Finishing...");
        }

        // Countdown (station remaining).
        {
            char cd[16];
            fmt_countdown(cd, v.countdown_s);
            lv_label_set_text(lbl_prog_cd, cd);
        }

        // Resume line: only while paused.
        if (v.paused) {
            int rs = v.pause_remaining_s;
            if (rs < 0) rs = 0;
            snprintf(buf, sizeof(buf), "Resumes in %d:%02d", rs / 60, rs % 60);
            lv_label_set_text(lbl_prog_resume, buf);
            obj_set_hidden(lbl_prog_resume, false);
        } else {
            obj_set_hidden(lbl_prog_resume, true);
        }

        // Pause button label.
        if (lbl_prog_pause_txt) {
            lv_label_set_text(lbl_prog_pause_txt, v.paused ? "Resume" : "Pause");
        }

        // ---- Right-side queue list -------------------------------------
        lv_label_set_text(lbl_qlist_hdr, prog_name);
        {
            int tr = pr.total_remaining_seconds;
            if (tr < 0) tr = 0;
            snprintf(buf, sizeof(buf), "%d:%02d left", tr / 60, tr % 60);
            lv_label_set_text(lbl_qlist_total, buf);
        }

        // Window the queue around the current station, keeping ~2 completed
        // rows visible above it for context. Chevron hints flag hidden rows.
        const auto& q = pr.queue;
        const int qn = static_cast<int>(q.size());
        int cur = 0;
        for (int i = 0; i < qn; ++i) {
            if (q[i].sid == pr.current_sid) { cur = i; break; }
        }
        int win_start = 0;
        if (qn > MAX_QROWS) {
            win_start = cur - 2;  // keep two done rows visible above current
            if (win_start < 0) win_start = 0;
            if (win_start > qn - MAX_QROWS) win_start = qn - MAX_QROWS;
        }
        const bool more_above = win_start > 0;
        const bool more_below = (win_start + MAX_QROWS) < qn;
        obj_set_hidden(qfade_top, !more_above);
        obj_set_hidden(qfade_bottom, !more_below);

        for (int r = 0; r < MAX_QROWS; ++r) {
            const int qi = win_start + r;
            if (qi < qn) {
                const auto& e = q[qi];
                const bool is_current = (e.sid == pr.current_sid);
                const bool is_done = e.done;

                // Current station shows a play glyph while running; when the
                // queue is paused it flips to a pause glyph so the list mirrors
                // the paused state (and the "Resume" button / amber status word).
                const char* current_mark =
                    v.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
                const char* mark = is_current ? current_mark
                                   : (is_done ? LV_SYMBOL_OK : "");
                lv_label_set_text(qrow_mark[r], mark);
                lv_obj_set_style_text_color(qrow_mark[r],
                    hex_color(is_current ? CLR_TEAL : CLR_MUTED), 0);

                const char* nm =
                    (e.sid >= 0 && e.sid < static_cast<int>(stns.size()))
                    ? stns[e.sid].name.c_str() : "Station";
                lv_label_set_text(qrow_name[r], nm);
                lv_obj_set_style_text_color(qrow_name[r],
                    hex_color(is_done ? CLR_MUTED : CLR_TEXT), 0);

                // Queue rows always show the station's FULL configured
                // duration (static) — the live per-station countdown lives in
                // the big ticker on the left, so nothing counts down here.
                int secs = e.total_seconds;
                if (secs < 0) secs = 0;
                snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
                lv_label_set_text(qrow_dur[r], buf);
                lv_obj_set_style_text_color(qrow_dur[r],
                    hex_color(is_current ? CLR_TEAL : CLR_MUTED), 0);

                obj_set_hidden(qrow_mark[r], false);
                obj_set_hidden(qrow_name[r], false);
                obj_set_hidden(qrow_dur[r], false);
            } else {
                obj_set_hidden(qrow_mark[r], true);
                obj_set_hidden(qrow_name[r], true);
                obj_set_hidden(qrow_dur[r], true);
            }
        }
    }

    // M9: Programs list content
    if (show_programs) {
        const auto& jp = g_ps->program_list();
        const int nprogs = static_cast<int>(jp.programs.size());
        const int page   = v.prog_list_page;
        const int start  = page * MAX_PROG_ROWS;
        const int total_pages = (nprogs + MAX_PROG_ROWS - 1) / MAX_PROG_ROWS;

        for (int r = 0; r < MAX_PROG_ROWS; ++r) {
            const int idx = start + r;
            if (idx < nprogs) {
                const auto& prog = jp.programs[idx];
                const int pid = idx + 1;
                const bool en = prog.enabled;

                // Enable/disable icon + name. Different SHAPES per state so
                // disabled doesn't just read as a dimmer "on": a power glyph
                // (teal) when enabled, a flat dash (muted) when disabled. The
                // dimmed name and Enable/Disable button reinforce it.
                lv_label_set_text(prog_row_icon[r],
                                  en ? LV_SYMBOL_POWER : LV_SYMBOL_MINUS);
                lv_obj_set_style_text_color(prog_row_icon[r],
                    hex_color(en ? CLR_TEAL : CLR_MUTED), 0);
                lv_label_set_text(prog_row_name[r], prog.name.c_str());
                lv_obj_set_style_text_color(prog_row_name[r],
                    hex_color(en ? CLR_TEXT : CLR_MUTED), 0);

                // Toggle button label
                lv_obj_t* tog_lbl =
                    static_cast<lv_obj_t*>(lv_obj_get_child(prog_row_btn_toggle[r], 0));
                if (tog_lbl) lv_label_set_text(tog_lbl, en ? "Disable" : "Enable");
                set_prog_pid(prog_row_btn_toggle[r], pid);

                // Next-run + zones/runtime meta line.
                {
                    char nr_buf[64];
                    char meta[32];
                    // "N zones • MM min" summary from /jp durations.
                    const int zones = prog.station_count();
                    const int mins  = (prog.total_seconds() + 59) / 60;
                    snprintf(meta, sizeof(meta),
                             "%d zones " LV_SYMBOL_BULLET " %d min", zones, mins);

                    // Compute the next run from the schedule regardless of the
                    // enabled flag (next_run() short-circuits on disabled, so
                    // evaluate an enabled copy to show *when it would run*).
                    auto sched = prog;
                    sched.enabled = true;
                    const long nr = osp::next_run(sched, v.ctrl_devt,
                                                  v.sunrise_min, v.sunset_min);
                    char when[32];
                    if (nr < 0) {
                        snprintf(when, sizeof(when), "Not scheduled");
                    } else {
                        // Calendar-day delta (not elapsed 24h periods).
                        const long nr_day   = nr / 86400;
                        const long dev_day  = v.ctrl_devt / 86400;
                        const long days_until = nr_day - dev_day;
                        const long tod = nr % 86400;
                        int h = static_cast<int>(tod / 3600);
                        const int m = static_cast<int>((tod % 3600) / 60);
                        const char* ap = (h < 12) ? "AM" : "PM";
                        int h12 = h % 12; if (h12 == 0) h12 = 12;

                        if (days_until <= 0) {
                            snprintf(when, sizeof(when), "Today %d:%02d %s",
                                     h12, m, ap);
                        } else if (days_until == 1) {
                            snprintf(when, sizeof(when), "Tomorrow %d:%02d %s",
                                     h12, m, ap);
                        } else if (days_until < 7) {
                            // Weekday abbrev; 1970-01-01 was a Thursday (=4).
                            static const char* kWd[7] =
                                {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                            const int wd = static_cast<int>((nr_day + 4) % 7);
                            snprintf(when, sizeof(when), "%s %d:%02d %s",
                                     kWd[wd], h12, m, ap);
                        } else {
                            snprintf(when, sizeof(when), "+%ld days %d:%02d %s",
                                     days_until, h12, m, ap);
                        }
                    }

                    // Next-run + meta on one line. State is shown by the icon
                    // + dimming, so disabled programs are NOT tagged in text —
                    // we still compute when they *would* run for reference.
                    snprintf(nr_buf, sizeof(nr_buf),
                             "%s " LV_SYMBOL_BULLET " %s", when, meta);
                    lv_label_set_text(prog_row_next[r], nr_buf);
                }

                // Run button pid
                set_prog_pid(prog_row_btn_run[r], pid);

                obj_set_hidden(prog_rows[r], false);
            } else {
                obj_set_hidden(prog_rows[r], true);
            }
        }

        // Pager: dots (indicators) + ‹ › arrows, only when >1 page.
        const bool need_pager = total_pages > 1;
        // Re-centre the visible dot cluster between the arrows (screen centre),
        // sized to the live page count so it stays centred regardless of how
        // many pages exist.
        const int dots_w = need_pager
            ? total_pages * PROG_DOT_W + (total_pages - 1) * PROG_DOT_GAP
            : 0;
        const int dots_x0 = (SCREEN_W - dots_w) / 2;
        for (int d = 0; d < MAX_PROG_PAGES; ++d) {
            if (!prog_page_dots[d]) continue;
            obj_set_hidden(prog_page_dots[d], !need_pager || d >= total_pages);
            if (need_pager && d < total_pages) {
                lv_obj_set_x(prog_page_dots[d],
                             dots_x0 + d * (PROG_DOT_W + PROG_DOT_GAP));
                lv_obj_set_style_bg_color(prog_page_dots[d],
                    hex_color(d == page ? CLR_TEAL : CLR_LINE), 0);
            }
        }
        if (prog_page_prev && prog_page_next) {
            obj_set_hidden(prog_page_prev, !need_pager);
            obj_set_hidden(prog_page_next, !need_pager);
            if (need_pager) {
                // Dim (but keep tappable — setter clamps) at the ends. make_btn
                // set the color on the child label, so target that.
                lv_obj_t* pl = lv_obj_get_child(prog_page_prev, 0);
                lv_obj_t* nl = lv_obj_get_child(prog_page_next, 0);
                if (pl) lv_obj_set_style_text_color(pl,
                    hex_color(page <= 0 ? CLR_MUTED : CLR_TEXT), 0);
                if (nl) lv_obj_set_style_text_color(nl,
                    hex_color(page >= total_pages - 1 ? CLR_MUTED : CLR_TEXT), 0);
            }
        }
    }

    // Run-time value (only meaningful when right panel is visible)
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
        const int sid = get_pill_sid(stn_pills[i]);
        const bool active = (running && sid == v.running_sid) ||
                            (prog_running && sid == v.prog_run.current_sid);
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

    {
        StateLock lock;
        if (lock && g_ps) g_ps->set_refreshing(true);
    }

    osp::JnData jn;
    if (!g_client->fetch_jn(jn)) {
        apply_link_error(now_ms, failures);
        StateLock lock;
        if (lock && g_ps) {
            g_ps->set_station_list_loaded(false);
            g_ps->set_refreshing(false);
        }
        return false;
    }

    osp::JoData jo;
    if (!g_client->fetch_jo(jo)) {
        apply_link_error(now_ms, failures);
        StateLock lock;
        if (lock && g_ps) {
            g_ps->set_station_list_loaded(false);
            g_ps->set_refreshing(false);
        }
        return false;
    }

    {
        StateLock lock;
        if (!(lock && g_ps)) return false;
        g_model.load(jn.snames, jn.stn_dis, jo.mas, jo.mas2);
        ++g_model_version;
        g_ps->set_station_list_loaded(true);
        g_ps->set_controller_identity(jo, g_os_host.c_str());
        g_ps->on_link_connected(now_ms);
        g_ps->set_refreshing(false);
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
    } else if (desired.kind == osp::IntentKind::RunProgram) {
        ok = g_client->run_program(desired.sid);
    } else if (desired.kind == osp::IntentKind::SetProgramEnabled) {
        ok = g_client->set_program_enabled(desired.sid, desired.seconds != 0);
        if (ok) g_jp_needs_refresh = true;
    } else if (desired.kind == osp::IntentKind::Pause) {
        ok = g_client->pause(600);
    } else if (desired.kind == osp::IntentKind::ProgramAdvance) {
        ok = g_client->skip_station(desired.sid);
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

        // Bench screen capture runs on this (LVGL-owning) task. Force a
        // SYNCHRONOUS full-screen redraw with lv_refr_now: it renders every
        // band and invokes disp_flush_cb (which tees each strip) before it
        // returns, since our flush is synchronous. That makes frame completion
        // deterministic — we emit the \x02END\n terminator right after the
        // redraw instead of trying to detect the bottom row inside the flush.
        if (g_capture_request && !g_capture_active) {
            g_capture_request = false;
            g_capture_active = true;
            if (g_log_client && g_log_client.connected()) {
                char hdr[32];
                const int n = snprintf(hdr, sizeof(hdr), "\x02SHOT %d %d 565\n",
                                       SCREEN_W, SCREEN_H);
                capture_write_all(reinterpret_cast<const uint8_t*>(hdr), n);
            }
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(lv_display_get_default());
            if (g_log_client && g_log_client.connected()) {
                // NOTE: write the STX explicitly, NOT as part of the literal
                // "\x02END\n". A \x hex escape consumes every following hex
                // digit, and 'E' is one — so "\x02END\n" compiles to the single
                // byte 0x2E ('.') plus "ND\n", i.e. ".ND\n", and the real STX
                // (0x02) terminator never went on the wire. (SHOT/STRIP escape
                // fine only because 'S' is not a hex digit.) Use adjacent string
                // literals so 0x02 is its own byte.
                static const char kEnd[] = "\x02" "END\n";
                capture_write_all(reinterpret_cast<const uint8_t*>(kEnd), 5);
                g_log_client.flush();
            }
            g_capture_active = false;
            g_capture_suppress_log = false;
        }

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
    }
}

// Emit one [HB] observability line (fix #60). Called from network_task (core 0)
// so the TeeSerial write shares the core that owns g_log_client — emitting from
// loop() (core 1) meant HB never reached the TCP dev-log because of a cross-core
// WiFiClient visibility race (fix #79); UART0 still saw it, masking the gap.
static void emit_heartbeat_log() {
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
                  g_sleeping_snapshot ? 1 : 0,
                  static_cast<unsigned long>(g_idle_ms_snapshot),
                  static_cast<unsigned long>(g_sleep_to_ms_snapshot),
                  static_cast<unsigned int>(ESP.getFreeHeap()),
                  static_cast<unsigned int>(ui_hwm),
                  static_cast<unsigned int>(net_hwm));
}

static void network_task(void* /*arg*/) {
    uint32_t next_jn_attempt_ms = 0;
    uint32_t jn_retry_ms = JN_RETRY_INITIAL_MS;
    uint32_t last_jc_poll_ms = 0;
    uint32_t last_hb_ms = 0;
    int link_failures = 0;
    bool station_list_loaded = false;

    for (;;) {
        ++g_net_beat;
        const uint32_t now = millis();

        dev_loop_handle();

        // Heartbeat is emitted here (core 0) rather than from loop() so it
        // reaches the TCP dev-log client (fix #79). Runs before the offline
        // early-continue so it keeps beating while the link is down.
        if (g_dev_log_enabled && (now - last_hb_ms) >= HEARTBEAT_LOG_INTERVAL_MS) {
            last_hb_ms = now;
            emit_heartbeat_log();
        }

        if (WiFi.status() != WL_CONNECTED || !g_client) {
            StateLock lock;
            if (lock && g_ps) {
                g_ps->on_link_offline(now);
                g_ps->set_refreshing(false);
                cache_phase_snapshot_unlocked();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!station_list_loaded && now >= next_jn_attempt_ms) {
            if (refresh_station_list(now, &link_failures)) {
                station_list_loaded = true;
                jn_retry_ms = JN_RETRY_INITIAL_MS;

                // Fetch program list alongside station list.
                osp::JpData jp;
                if (g_client->fetch_jp(jp)) {
                    StateLock lock;
                    if (lock && g_ps) g_ps->set_program_list(jp);
                    Serial.printf("Programs: %d loaded\n",
                                  static_cast<int>(jp.programs.size()));
                }
            } else {
                next_jn_attempt_ms = now + jn_retry_ms;
                jn_retry_ms = std::min<uint32_t>(jn_retry_ms * 2, JN_RETRY_MAX_MS);
            }
        }

        // Refresh /jp after a SetProgramEnabled delivery.
        if (g_jp_needs_refresh && g_client) {
            g_jp_needs_refresh = false;
            osp::JpData jp;
            if (g_client->fetch_jp(jp)) {
                StateLock lock;
                if (lock && g_ps) g_ps->set_program_list(jp);
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

    // Battery sense ADC: ~0–3.1 V range so VBAT/2 (1.5–2.1 V) sits in the
    // linear region. Uses the eFuse Vref calibration via analogReadMilliVolts.
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);

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
    int saved_sleep_s = DEFAULT_SLEEP_S;
    load_config_from_nvs(nullptr, nullptr, nullptr, nullptr, nullptr, &saved_rt,
                         nullptr, &saved_aa, &saved_sleep_s);

    // ---- Provision / connect (M4 seam) ----------------------------------
    ensure_network_config();
    load_config_from_nvs(nullptr, nullptr, nullptr, nullptr, nullptr, &saved_rt,
                         nullptr, &saved_aa, &saved_sleep_s);

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
            g_ps->set_sleep_timeout_ms((uint32_t)saved_sleep_s * 1000UL);
            g_ps->set_controller_identity(osp::JoData{}, g_os_host.c_str());
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
    // The dev-log heartbeat now emits from network_task (core 0) so it reaches
    // the TCP client (fix #79). Nothing else runs on the Arduino loop task.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
