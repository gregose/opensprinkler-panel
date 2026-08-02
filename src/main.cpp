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
#include "history_model.h"
#include "os_client.h"
#include "panel_config.h"
#include "panel_state.h"
#include "station_model.h"
#include "battery_monitor.h"
#include "ui_font_countdown_48.h"
#include "ui_theme.h"
#include "top_bar.h"
#include "panel_screen.h"
#include "widgets.h"

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

// SCREEN_W / SCREEN_H now live in lib/ui/ui_theme.h (shared with the sim).
// Draw buffer: 480x4 pixels - keeps BSS small on the no-PSRAM ESP32.
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

// Visual tokens (CLR_*), hex_color() and obj_set_hidden() now live in
// lib/ui/ui_theme.h so the firmware and the host sim share one definition.

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
static std::vector<osp::HistoryRecord> g_history_records;
static std::vector<osp::ui::HistoryEntry> g_history_entries;
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
// Protected by g_state_mutex. The UI task sets this on each History open and
// the network task clears it when claiming the refresh.
static bool g_history_fetch_requested = false;

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
// UI screen state
// ---------------------------------------------------------------------------
// All LVGL screen geometry lives in lib/ui (osp::ui::*, linked by firmware AND
// the host sim). main.cpp holds ONE composite handle bundle; builders return it
// and update_panel_screen drives per-frame updates. No screen geometry here.
static osp::ui::PanelScreen g_screen;

// Smoothed LiPo monitor fed from PIN_BAT_ADC (pure logic in lib/battery_monitor).
static osp::BatteryMonitor g_batt;

// M9: signal that /jp should be re-fetched after a SetProgramEnabled delivery
static volatile bool g_jp_needs_refresh = false;


// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------
// Shared running-screen action handlers. Next dispatches by mode (program skip
// vs manual advance); Pause and Stop are already mode-agnostic in PanelState.
static void ev_next(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (!(lock && g_ps)) return;
    if (g_ps->view().phase == osp::Phase::ProgramRunning) {
        g_ps->program_advance_intent();
    } else {
        g_ps->advance();
    }
}
static void ev_pause(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->pause_toggle_intent();
}
static void ev_stop_run(lv_event_t* e) {
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
        g_ps->select_station(osp::ui::get_obj_id(pill));
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
    const int pid = osp::ui::get_obj_id(btn);
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
    const int pid = osp::ui::get_obj_id(btn);
    // Intents take a 0-based program index (/mp?pid= is 0-based in the API).
    if (pid >= 1) g_ps->run_program_intent(pid - 1);
}

// #127: History screen navigation. PR1 has no /jl data yet, so the list renders
// empty on hardware; the pager setter is fed a single-page total for now.
static void ev_open_history(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) {
        const bool was_open = g_ps->view().showing_history;
        g_ps->open_history();
        if (!was_open && g_ps->view().showing_history) {
            g_history_fetch_requested = true;
        }
    }
}
static void ev_close_history(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (lock && g_ps) g_ps->close_history();
}
static void ev_hist_page_prev(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (!(lock && g_ps)) return;
    const int total_pages = g_history_entries.empty()
        ? 0
        : (static_cast<int>(g_history_entries.size()) +
           MAX_HIST_ROWS - 1) / MAX_HIST_ROWS;
    g_ps->set_hist_list_page(g_ps->view().hist_list_page - 1, total_pages);
}
static void ev_hist_page_next(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    StateLock lock;
    if (!(lock && g_ps)) return;
    const int total_pages = g_history_entries.empty()
        ? 0
        : (static_cast<int>(g_history_entries.size()) +
           MAX_HIST_ROWS - 1) / MAX_HIST_ROWS;
    g_ps->set_hist_list_page(g_ps->view().hist_list_page + 1, total_pages);
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

// Screen geometry constants (GRID_H, ACTION_Y, RIGHT_W, etc.) now live in
// lib/ui/ui_layout.h, shared byte-for-byte between firmware and the host sim.

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

// ---------------------------------------------------------------------------
// UI wiring (thin glue over lib/ui)
// ---------------------------------------------------------------------------
// Assemble the firmware event handlers into the portable Callbacks bundle that
// build_panel_screen() attaches to the interactive widgets. Keeping the handlers
// here (they touch g_ps / NVS / network) lets lib/ui stay dependency-free.
static osp::ui::Callbacks make_ui_callbacks() {
    osp::ui::Callbacks cbs;
    cbs.on_screen_pressed = ev_touch_any;
    cbs.on_pill           = ev_pill;
    cbs.on_next           = ev_next;
    cbs.on_pause          = ev_pause;
    cbs.on_stop           = ev_stop_run;
    cbs.on_rt_minus       = ev_rt_minus;
    cbs.on_rt_plus        = ev_rt_plus;
    cbs.on_auto_adv       = ev_auto_adv;
    cbs.on_open_programs  = ev_open_programs;
    cbs.on_close_programs = ev_close_programs;
    cbs.on_prog_page_prev = ev_prog_page_prev;
    cbs.on_prog_page_next = ev_prog_page_next;
    cbs.on_prog_toggle    = ev_prog_toggle_enabled;
    cbs.on_prog_run       = ev_prog_run;
    cbs.on_open_history   = ev_open_history;
    cbs.on_close_history  = ev_close_history;
    cbs.on_hist_page_prev = ev_hist_page_prev;
    cbs.on_hist_page_next = ev_hist_page_next;
    return cbs;
}

static void build_ui() {
    g_screen = osp::ui::build_panel_screen(lv_screen_active(), make_ui_callbacks());
}

static void build_grid() {
    osp::ui::build_station_grid(g_screen, g_model);
}

static void ui_update() {
    if (!g_ps) return;
    const osp::PanelView& v = g_ps->view();

    // Firmware-only status the pure view model can't carry: panel Wi-Fi, battery
    // gauge sample, configured host string. Everything else comes from PanelView.
    poll_battery();
    const int battery_percent =
        g_batt_override_percent >= 0 ? g_batt_override_percent
                                     : g_batt.percent();
    osp::ui::HostStatus host;
    host.panel_rssi_bars = osp::rssi_to_bars(WiFi.RSSI());
    host.panel_connected = WiFi.status() == WL_CONNECTED;
    host.battery_percent = battery_percent;
    host.battery_tier    = osp::battery_tier_from_percent(battery_percent);
    host.host_name       = g_os_host.c_str();

    osp::ui::HistoryView history;
    history.entries = g_history_entries.empty() ? nullptr
                                                : g_history_entries.data();
    history.count = static_cast<int>(g_history_entries.size());
    history.page = v.hist_list_page;

    osp::ui::update_panel_screen(g_screen, v, g_model, g_ps->program_list(),
                                 history, host);

    // Backlight is a hardware side-effect, kept out of the portable update.
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

static bool retain_visible_history_log(const osp::LogEntry& entry) {
    if (entry.pid == 0) {
        return entry.event_code == "s1" || entry.event_code == "s2" ||
               entry.event_code == "rd";
    }
    return entry.pid == 99 || entry.pid == 254 ||
           (entry.pid >= 1 && entry.pid <= 250);
}

static osp::LogTransport make_jl_transport() {
    return [](const std::string& url, std::size_t max_entries,
              std::vector<osp::LogEntry>& out) -> osp::OsResult {
        HTTPClient http;
        http.setConnectTimeout(1500);
        http.setTimeout(1500);
        if (!http.begin(url.c_str())) return osp::OsResult::NetworkError;
        const int code = http.GET();
        if (code != HTTP_CODE_OK) {
            http.end();
            return osp::OsResult::NetworkError;
        }

        osp::JlParser parser(out, max_entries, retain_visible_history_log);
        WiFiClient* stream = http.getStreamPtr();
        if (!stream) {
            http.end();
            return osp::OsResult::NetworkError;
        }
        int remaining = http.getSize();
        uint32_t last_progress_ms = millis();
        std::string error_body;
        error_body.reserve(64);
        bool parser_valid = true;
        uint8_t buffer[128];

        while (remaining > 0 || remaining == -1) {
            const int available = stream->available();
            if (available <= 0) {
                if (parser.finish() ||
                    !http.connected() ||
                    (millis() - last_progress_ms) >= 1500) {
                    break;
                }
                delay(1);
                continue;
            }

            std::size_t to_read =
                std::min<std::size_t>(available, sizeof(buffer));
            if (remaining > 0) {
                to_read = std::min<std::size_t>(
                    to_read, static_cast<std::size_t>(remaining));
            }
            const std::size_t read = stream->readBytes(buffer, to_read);
            if (read == 0) continue;
            last_progress_ms = millis();
            if (remaining > 0) remaining -= static_cast<int>(read);

            if (error_body.size() < 64) {
                const std::size_t copy =
                    std::min<std::size_t>(read, 64 - error_body.size());
                error_body.append(reinterpret_cast<const char*>(buffer), copy);
            }
            if (parser_valid) {
                parser_valid = parser.feed(
                    reinterpret_cast<const char*>(buffer), read);
            }
            if (parser.finish()) break;
        }

        osp::OsResult result = osp::OsResult::NetworkError;
        if (parser_valid && parser.finish()) {
            result = osp::OsResult::Ok;
        } else {
            result = osp::parse_result(error_body);
        }
        http.end();
        return result;
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

static osp::ui::HistoryEntry::Kind ui_history_kind(osp::HistoryKind kind) {
    using UiKind = osp::ui::HistoryEntry::Kind;
    switch (kind) {
        case osp::HistoryKind::ProgramRun: return UiKind::ProgramRun;
        case osp::HistoryKind::ManualRun:  return UiKind::ManualRun;
        case osp::HistoryKind::RunOnce:    return UiKind::RunOnce;
        case osp::HistoryKind::RainDelay:  return UiKind::RainDelay;
        case osp::HistoryKind::Sensor1:    return UiKind::Sensor1;
        case osp::HistoryKind::Sensor2:    return UiKind::Sensor2;
    }
    return UiKind::ProgramRun;
}

static void rebuild_ui_history_unlocked() {
    g_history_entries.clear();
    g_history_entries.reserve(g_history_records.size());
    for (const osp::HistoryRecord& record : g_history_records) {
        osp::ui::HistoryEntry entry;
        entry.name = record.name.c_str();
        entry.dur_s = record.duration_s;
        entry.when = record.when.c_str();
        entry.kind = ui_history_kind(record.kind);
        entry.tag = record.tag.empty() ? nullptr : record.tag.c_str();
        g_history_entries.push_back(entry);
    }
}

static bool refresh_history(uint32_t now_ms, int* failures) {
    if (!g_client) return false;

    std::vector<osp::LogEntry> logs;
    if (!g_client->fetch_jl(
            logs, 30,
            static_cast<std::size_t>(osp::ui::HISTORY_MAX_RECORDS))) {
        apply_link_error(now_ms, failures);
        return false;
    }

    std::vector<std::string> station_names;
    osp::JpData programs;
    uint32_t controller_now = 0;
    {
        StateLock lock;
        if (!(lock && g_ps)) return false;
        station_names.reserve(g_model.stations().size());
        for (const osp::Station& station : g_model.stations()) {
            station_names.push_back(station.name);
        }
        programs = g_ps->program_list();
        controller_now = static_cast<uint32_t>(
            std::max(0L, g_ps->view().ctrl_devt));
    }
    if (controller_now == 0) return false;

    std::vector<osp::HistoryRecord> records = osp::build_history_records(
        logs, station_names, programs, controller_now,
        static_cast<std::size_t>(osp::ui::HISTORY_MAX_RECORDS));

    {
        StateLock lock;
        if (!(lock && g_ps)) return false;
        g_history_records = std::move(records);
        rebuild_ui_history_unlocked();
        const int total_pages = g_history_entries.empty()
            ? 0
            : (static_cast<int>(g_history_entries.size()) +
               MAX_HIST_ROWS - 1) / MAX_HIST_ROWS;
        g_ps->set_hist_list_page(g_ps->view().hist_list_page, total_pages);
        g_ps->on_link_connected(now_ms);
        cache_phase_snapshot_unlocked();
    }
    *failures = 0;
    Serial.printf("History: %d visible records from %d log rows\n",
                  static_cast<int>(g_history_entries.size()),
                  static_cast<int>(logs.size()));
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

        bool refresh_history_now = false;
        {
            StateLock lock;
            if (lock && g_history_fetch_requested) {
                g_history_fetch_requested = false;
                refresh_history_now = true;
            }
        }
        if (refresh_history_now) {
            bool has_controller_time = false;
            {
                StateLock lock;
                has_controller_time =
                    lock && g_ps && g_ps->view().ctrl_devt > 0;
            }
            if (!has_controller_time &&
                poll_controller(now, &link_failures)) {
                last_jc_poll_ms = now;
                has_controller_time = true;
            }
            if (has_controller_time) {
                refresh_history(now, &link_failures);
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
        host_url.c_str(), g_pw_md5.c_str(), make_http_transport(),
        make_jl_transport()));

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
