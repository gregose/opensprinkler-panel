// Diagnostic / bring-up firmware for the ESP32-3248S035R (CYD).
// Serial-driven menu (115200 baud) for staged hardware verification.
//
// Send a single character over the serial console to exercise each layer:
//   b  Boot banner (M0)
//   d  Display color cycle (M1)
//   i  Toggle invertDisplay (M1)
//   r  Step display rotation (M1)
//   t  Raw touch stream (M2, 'q' to stop)
//   c  3-point touch calibration (M2)
//   l  LVGL smoke test (M3, 'q' to stop)
//   n  Print NVS config
//   s  Set NVS key (interactive — see tools/seed-nvs.sh for scripted use)
//   x  Clear NVS namespace
//   ?  Help
//
// Hardware constants, NVS keys, and LVGL buffer setup mirror src/main.cpp
// exactly so both firmwares share the same NVS namespace and display config.
// Do NOT modify src/main.cpp — this file is the sole source for diag logic.

#include <Arduino.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <esp_system.h>

#include <TFT_eSPI.h>
#include <lvgl.h>

// ---------------------------------------------------------------------------
// Git SHA — injected by CI via -D FW_GIT_SHA (GIT_SHA env var at build time).
// Falls back to "dev" when the env var is unset (empty string from sysenv).
// ---------------------------------------------------------------------------
#ifndef FW_GIT_SHA
#  define FW_GIT_SHA ""
#endif
static inline const char* fw_git_sha() {
    return (FW_GIT_SHA[0] != '\0') ? FW_GIT_SHA : "dev";
}

// ---------------------------------------------------------------------------
// Hardware constants — keep identical to src/main.cpp.
// ---------------------------------------------------------------------------
static constexpr int PIN_BACKLIGHT   = 27;
static constexpr int LEDC_CHANNEL    = 0;
static constexpr int LEDC_FREQ_HZ    = 5000;
static constexpr int LEDC_RES_BITS   = 8;

static constexpr int SCREEN_W        = 480;
static constexpr int SCREEN_H        = 320;
static constexpr int DRAW_BUF_LINES  = 4;

// NVS namespace and keys — must match src/main.cpp.
static constexpr const char* NVS_NS    = "osp-panel";
static constexpr const char* NVS_SSID  = "wifi_ssid";
static constexpr const char* NVS_PASS  = "wifi_pass";
static constexpr const char* NVS_HOST  = "os_host";
static constexpr const char* NVS_PWMD5 = "os_pw_md5";
static constexpr const char* NVS_TOUCHCAL = "touch_cal";  // uint16_t calData[5] blob

// Timeout for the NVS interactive setter (ms).
static constexpr unsigned long NVS_INPUT_TIMEOUT_MS = 30000;


// ---------------------------------------------------------------------------
// Hardware objects
// ---------------------------------------------------------------------------
static TFT_eSPI tft;

// LVGL draw buffer — same sizing as main.cpp (no PSRAM on this board).
static lv_color_t draw_buf[SCREEN_W * DRAW_BUF_LINES];

// Display state for toggle commands.
static uint8_t g_rotation = 1;  // landscape (matches main.cpp setup())
static bool    g_invert   = false;

// ---------------------------------------------------------------------------
// LVGL callbacks — identical to src/main.cpp so M3 uses the same code path.
// ---------------------------------------------------------------------------
static void disp_flush_cb(lv_display_t* disp, const lv_area_t* area,
                           uint8_t* px_map) {
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushPixels(reinterpret_cast<const uint16_t*>(px_map), w * h);
    tft.endWrite();
    lv_display_flush_ready(disp);
}

static void touchpad_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    uint16_t tx = 0, ty = 0;
    const bool pressed = tft.getTouch(&tx, &ty);
    data->state   = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = static_cast<int32_t>(tx);
    data->point.y = static_cast<int32_t>(ty);
}

// ---------------------------------------------------------------------------
// M0 — Boot banner
// ---------------------------------------------------------------------------
static void m0_boot_banner() {
    Serial.println("\n=== OpenSprinkler Panel — Diagnostic Firmware ===");
    Serial.printf("Git SHA  : %s\n", fw_git_sha());
    Serial.printf("Built    : %s %s\n", __DATE__, __TIME__);
    Serial.printf("Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());

    const esp_reset_reason_t reason = esp_reset_reason();
    const char* rs = "unknown";
    switch (reason) {
        case ESP_RST_POWERON:   rs = "power-on";   break;
        case ESP_RST_EXT:       rs = "external";   break;
        case ESP_RST_SW:        rs = "software";   break;
        case ESP_RST_PANIC:     rs = "panic";      break;
        case ESP_RST_INT_WDT:   rs = "int-WDT";   break;
        case ESP_RST_TASK_WDT:  rs = "task-WDT";  break;
        case ESP_RST_WDT:       rs = "other-WDT"; break;
        case ESP_RST_DEEPSLEEP: rs = "deep-sleep"; break;
        case ESP_RST_BROWNOUT:  rs = "brownout";   break;
        case ESP_RST_SDIO:      rs = "SDIO";       break;
        default: break;
    }
    Serial.printf("Reset    : %s\n", rs);
}

// ---------------------------------------------------------------------------
// M1 — Display test
// ---------------------------------------------------------------------------
static void m1_draw_crosshair_tft(int x, int y, uint16_t color) {
    tft.drawLine(x - 12, y, x + 12, y, color);
    tft.drawLine(x, y - 12, x, y + 12, color);
    tft.drawCircle(x, y, 6, color);
}

static void m1_cycle_colors() {
    struct { uint32_t fg; uint32_t bg; const char* name; } fills[] = {
        {TFT_WHITE, TFT_RED,   "RED"},
        {TFT_WHITE, TFT_GREEN, "GREEN"},
        {TFT_WHITE, TFT_BLUE,  "BLUE"},
        {TFT_BLACK, TFT_WHITE, "WHITE"},
        {TFT_WHITE, TFT_BLACK, "BLACK"},
    };
    for (auto& f : fills) {
        tft.fillScreen(f.bg);
        tft.setTextColor(f.fg, f.bg);
        tft.setTextSize(2);
        // Corner labels (TL/TR/BL/BR).
        tft.setCursor(4, 4);            tft.print("TL");
        tft.setCursor(SCREEN_W - 28, 4);           tft.print("TR");
        tft.setCursor(4, SCREEN_H - 20);           tft.print("BL");
        tft.setCursor(SCREEN_W - 28, SCREEN_H - 20); tft.print("BR");
        // Crosshairs at corners to verify offset.
        m1_draw_crosshair_tft(20, 20,                 f.fg);
        m1_draw_crosshair_tft(SCREEN_W - 20, 20,      f.fg);
        m1_draw_crosshair_tft(20, SCREEN_H - 20,      f.fg);
        m1_draw_crosshair_tft(SCREEN_W - 20, SCREEN_H - 20, f.fg);
        // Centered label.
        tft.setTextSize(3);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(f.name, SCREEN_W / 2, SCREEN_H / 2);
        tft.setTextDatum(TL_DATUM);
        Serial.printf("M1: %s\n", f.name);
        delay(800);
    }
    Serial.println("M1: color cycle done");
}

static void m1_toggle_invert() {
    g_invert = !g_invert;
    tft.invertDisplay(g_invert);
    Serial.printf("M1: invertDisplay=%s\n", g_invert ? "on" : "off");
}

static void m1_step_rotation() {
    g_rotation = (g_rotation + 1) % 4;
    tft.setRotation(g_rotation);
    Serial.printf("M1: rotation=%u\n", (unsigned)g_rotation);
    // Redraw a simple orientation marker so the result is visible.
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(4, 4);
    tft.printf("rot=%u", (unsigned)g_rotation);
    tft.setCursor(4, 30);
    tft.print("TL corner");
}

// ---------------------------------------------------------------------------
// M2 — Raw touch stream + calibration (TFT_eSPI built-in)
// ---------------------------------------------------------------------------
// Load a saved calData[5] blob from NVS and apply it via setTouch(). Returns
// true if a valid calibration was found and applied.
static bool load_touch_cal() {
    uint16_t calData[5] = {0};
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    const size_t got = prefs.getBytes(NVS_TOUCHCAL, calData, sizeof(calData));
    prefs.end();
    if (got == sizeof(calData)) {
        tft.setTouch(calData);
        Serial.printf("Touch: loaded calibration from NVS '%s'.\n", NVS_TOUCHCAL);
        return true;
    }
    Serial.printf("Touch: no saved calibration (run 'c' to calibrate).\n");
    return false;
}

static void m2_draw_crosshair(int x, int y, uint16_t color) {
    tft.drawLine(x - 10, y, x + 10, y, color);
    tft.drawLine(x, y - 10, x, y + 10, color);
    tft.drawCircle(x, y, 5, color);
}

static void m2_touch_stream() {
    Serial.println("M2: touch stream active (raw ADC x/y/z + mapped coords).");
    Serial.println("    Send 'q' to stop.");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.print("Touch stream — send 'q' to stop");

    while (true) {
        if (Serial.available()) {
            const char ch = (char)Serial.read();
            if (ch == 'q' || ch == 'Q') break;
        }
        uint16_t raw_x = 0, raw_y = 0;
        if (tft.getTouchRaw(&raw_x, &raw_y)) {
            const uint16_t z = tft.getTouchRawZ();
            uint16_t tx = 0, ty = 0;
            tft.getTouch(&tx, &ty);
            Serial.printf("M2: raw(%4u,%4u) z=%4u -> screen(%3u,%3u)\n",
                          (unsigned)raw_x, (unsigned)raw_y, (unsigned)z,
                          (unsigned)tx, (unsigned)ty);
            m2_draw_crosshair((int)tx, (int)ty, TFT_CYAN);
        }
        delay(50);
    }
    Serial.println("M2: touch stream stopped.");
}

static void m2_calibration() {
    // Use TFT_eSPI's built-in interactive calibration: it draws corner targets,
    // collects the taps, and fills a uint16_t calData[5] blob (xmin, ymin, xmax,
    // ymax, rotation-flag). We then apply it with setTouch() and persist it to
    // NVS so every firmware (this diag build + production M6) maps touch->pixel
    // identically. No custom calibration math — this is the standard XPT2046 path.
    uint16_t calData[5] = {0};

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.println("Touch calibration");
    tft.setTextSize(1);
    tft.println("");
    tft.println(" Tap each highlighted corner arrow.");
    Serial.println("M2 cal: tap the corner arrows as they appear...");

    tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 20);
    tft.setTouch(calData);

    // Persist the 10-byte blob to NVS.
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putBytes(NVS_TOUCHCAL, calData, sizeof(calData));
    prefs.end();

    Serial.print("M2 cal: calData[5] = { ");
    for (int i = 0; i < 5; ++i) Serial.printf("%u%s", (unsigned)calData[i], i < 4 ? ", " : " ");
    Serial.println("}");
    Serial.print("M2 cal: blob (hex) = ");
    const uint8_t* b = reinterpret_cast<const uint8_t*>(calData);
    for (size_t j = 0; j < sizeof(calData); ++j) Serial.printf("%02x", b[j]);
    Serial.println();
    Serial.printf("M2 cal: saved to NVS '%s'. Run 't' to verify tracking.\n", NVS_TOUCHCAL);

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.println("Calibration saved");
}

// ---------------------------------------------------------------------------
// M3 — LVGL smoke test
// ---------------------------------------------------------------------------
static bool      g_lvgl_ready = false;
static lv_obj_t* g_lbl_count  = nullptr;
static int        g_tick_count = 0;

static void btn_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        Serial.println("M3: button clicked (touch->LVGL path OK)");
    }
}

static void timer_cb(lv_timer_t* /*t*/) {
    ++g_tick_count;
    if (g_lbl_count) {
        char buf[24];
        snprintf(buf, sizeof(buf), "ticks: %d", g_tick_count);
        lv_label_set_text(g_lbl_count, buf);
    }
}

static void m3_lvgl_smoke() {
    if (!g_lvgl_ready) {
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
        g_lvgl_ready = true;
    }

    g_tick_count = 0;
    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Button.
    lv_obj_t* btn = lv_button_create(scr);
    lv_obj_set_size(btn, 140, 55);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Tap me (M3)");
    lv_obj_center(btn_lbl);

    // Counter label updated by timer.
    g_lbl_count = lv_label_create(scr);
    lv_label_set_text(g_lbl_count, "ticks: 0");
    lv_obj_set_style_text_color(g_lbl_count, lv_color_white(), 0);
    lv_obj_align(g_lbl_count, LV_ALIGN_CENTER, 0, 30);

    lv_timer_create(timer_cb, 500, nullptr);

    Serial.println("M3: LVGL smoke running (tap button, send 'q' to stop).");
    lv_timer_handler();

    uint32_t last_tick_ms = millis();
    while (true) {
        // Advance LVGL tick from Arduino millis().
        const uint32_t now = millis();
        lv_tick_inc(now - last_tick_ms);
        last_tick_ms = now;

        if (Serial.available()) {
            const char ch = (char)Serial.read();
            if (ch == 'q' || ch == 'Q') break;
        }
        lv_timer_handler();
        delay(5);
    }
    Serial.printf("M3: stopped after %d timer ticks.\n", g_tick_count);
    g_lbl_count = nullptr;
}

// ---------------------------------------------------------------------------
// NVS config commands
// ---------------------------------------------------------------------------
static void nvs_print() {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    Serial.printf("NVS namespace: %s\n", NVS_NS);
    Serial.printf("  %-12s = \"%s\"\n", NVS_SSID,  prefs.getString(NVS_SSID,  "(not set)").c_str());
    Serial.printf("  %-12s = \"%s\"\n", NVS_PASS,  prefs.getString(NVS_PASS,  "(not set)").c_str());
    Serial.printf("  %-12s = \"%s\"\n", NVS_HOST,  prefs.getString(NVS_HOST,  "(not set)").c_str());
    Serial.printf("  %-12s = \"%s\"\n", NVS_PWMD5, prefs.getString(NVS_PWMD5, "(not set)").c_str());
    prefs.end();
}

// Compute MD5 hex string using arduino-esp32's MD5Builder.
static String md5_hex(const String& plaintext) {
    MD5Builder md5;
    md5.begin();
    md5.add(plaintext);
    md5.calculate();
    return md5.toString();
}

// Interactive NVS setter.  The host tool tools/seed-nvs.sh drives this
// function by sending 's' followed by "key value\n" over the serial link.
static void nvs_set_interactive() {
    Serial.println("NVS> enter: <key> <value>");
    Serial.println("NVS> keys: wifi_ssid  wifi_pass  os_host  os_pw_md5");
    Serial.println("NVS> (os_pw_md5 accepts plaintext — MD5 computed on-device)");

    String line;
    const unsigned long deadline = millis() + NVS_INPUT_TIMEOUT_MS;
    while (millis() < deadline) {
        if (Serial.available()) {
            const char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (line.length() > 0) break;
            } else {
                line += c;
            }
        }
    }
    line.trim();
    if (line.isEmpty()) {
        Serial.println("NVS set: aborted (timeout or empty input).");
        return;
    }

    const int sp = line.indexOf(' ');
    if (sp < 0) {
        Serial.println("NVS set: invalid format — need '<key> <value>'.");
        return;
    }
    const String key = line.substring(0, sp);
    const String val = line.substring(sp + 1);

    Preferences prefs;
    prefs.begin(NVS_NS, false);
    if (key == NVS_SSID) {
        prefs.putString(NVS_SSID, val);
        Serial.printf("NVS: set %s = \"%s\"\n", NVS_SSID, val.c_str());
    } else if (key == NVS_PASS) {
        prefs.putString(NVS_PASS, val);
        Serial.printf("NVS: set %s = \"%s\"\n", NVS_PASS, val.c_str());
    } else if (key == NVS_HOST) {
        prefs.putString(NVS_HOST, val);
        Serial.printf("NVS: set %s = \"%s\"\n", NVS_HOST, val.c_str());
    } else if (key == NVS_PWMD5) {
        // Accept plaintext, store the MD5 hash — plaintext never persists.
        const String hashed = md5_hex(val);
        prefs.putString(NVS_PWMD5, hashed);
        Serial.printf("NVS: set %s = \"%s\" (MD5 of plaintext)\n",
                      NVS_PWMD5, hashed.c_str());
    } else {
        Serial.printf("NVS set: unknown key \"%s\".\n", key.c_str());
    }
    prefs.end();
}

static void nvs_clear() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();
    Serial.printf("NVS: cleared namespace \"%s\".\n", NVS_NS);
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------
static void print_help() {
    Serial.println("\n--- Diag menu (send one char) ---");
    Serial.println("  b  Boot banner (M0)");
    Serial.println("  d  Display color cycle (M1)");
    Serial.println("  i  Toggle invertDisplay (M1)");
    Serial.println("  r  Step display rotation (M1)");
    Serial.println("  t  Touch stream: raw ADC x/y/z + screen coords (M2, 'q' stop)");
    Serial.println("  c  Touch calibration (TFT_eSPI calibrateTouch, saves to NVS)");
    Serial.println("  l  LVGL smoke test: button + timer label (M3, 'q' stop)");
    Serial.println("  n  Print NVS config");
    Serial.println("  s  Set NVS key (interactive / tools/seed-nvs.sh)");
    Serial.println("  x  Clear NVS namespace");
    Serial.println("  ?  This help");
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);

    // Backlight on via LEDC PWM (GPIO27 per docs/03).
    ledcSetup(LEDC_CHANNEL, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledcAttachPin(PIN_BACKLIGHT, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 255);

    tft.init();
    tft.setRotation(g_rotation);
    tft.fillScreen(TFT_BLACK);
    load_touch_cal();

    m0_boot_banner();
    print_help();
}

void loop() {
    if (!Serial.available()) {
        delay(10);
        return;
    }
    const char cmd = (char)Serial.read();
    switch (cmd) {
        case 'b': m0_boot_banner();      break;
        case 'd': m1_cycle_colors();     break;
        case 'i': m1_toggle_invert();    break;
        case 'r': m1_step_rotation();    break;
        case 't': m2_touch_stream();     break;
        case 'c': m2_calibration();      break;
        case 'l': m3_lvgl_smoke();       break;
        case 'n': nvs_print();           break;
        case 's': nvs_set_interactive(); break;
        case 'x': nvs_clear();           break;
        case '?':
        case 'h': print_help();          break;
        default:
            if (cmd >= 0x20) {
                Serial.printf("Unknown command '%c'. Send '?' for help.\n", cmd);
            }
            break;
    }
}
