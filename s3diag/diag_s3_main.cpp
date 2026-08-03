// ===========================================================================
// S3 bring-up spike (issue #140) - Waveshare ESP32-S3-Touch-LCD-3.5 (non-B).
//
// DIAG ONLY. Fully additive. This file is compiled ONLY by the
// [env:s3-touch-35-diag] PlatformIO env (build_src_filter = -<*> +<../s3diag/>).
// It shares NOTHING with the production cyd-35r firmware or the cyd-35r-diag
// harness - it is a self-contained hardware bring-up sketch whose sole job is to
// retire the four highest-risk unknowns for the S3 port before any change to
// src/main.cpp:
//
//   1. AXP2101 PMIC  - which rails must be enabled to power the panel + touch,
//                      and whether the fuel gauge reports real SOC/charge/VBUS.
//   2. TCA9554       - the LCD reset choreography (reset is on an I2C expander
//                      pin, not a GPIO) that must run BEFORE the display init.
//   3. ST7796 @80MHz - the display driver we already ship (TFT_eSPI), on the new
//                      S3 SPI pins, at 80 MHz.
//   4. FT6336 touch  - capacitive touch over I2C (0x38) via SensorLib.
//
// Bring-up order (mirrors the vendor demos and the ESP-IDF BSP init order):
//   (a) Serial (USB-CDC) up, print a self-describing banner.
//   (b) I2C bus up on SDA IO8 / SCL IO7 (shared: PMIC, expander, touch, ...).
//   (c) AXP2101 FIRST - enable rails, disableTSPinMeasure(), print battery.
//   (d) Pulse LCD reset via TCA9554 pin 1 (low->high) before display init.
//   (e) ST7796 over TFT_eSPI + a small LVGL 9 smoke screen (label + shape).
//   (f) Poll one FT6336 touch point and print coordinates on each new touch.
//
// Vendor references (see issue #120 for links):
//   - AXP2101 rails : waveshareteam/ESP32-S3-Touch-LCD-3.5
//                     Arduino/examples/02_axp2101_example/02_axp2101_example.ino
//   - TCA9554 reset : .../examples/08_gfx_helloworld/08_gfx_helloworld.ino
//                     (lcd_reset(): write1(1,1)->write1(1,0)->write1(1,1))
//   - FT6336 touch  : .../examples/11_lvgl_arduino_v8/11_lvgl_arduino_v8.ino
//                     (TouchDrvFT6X36, getPoint(x,y,1))
//   NOTE: the vendor demos drive the panel with Arduino_GFX + LVGL 8. We keep
//   our shipping stack (TFT_eSPI + LVGL 9.2) - the reset/PMIC/touch bring-up is
//   identical, only the display glue differs.
//
// Bench validation (Greg flashes; this agent does not): see the PR body. In
// short: expect the banner + per-stage [ OK ]/[FAIL] lines and real battery
// numbers over serial, the LVGL smoke screen on the panel, and "touch (x,y)"
// lines when you tap.
// ===========================================================================

#include <Arduino.h>
#include <Wire.h>

// TFT_eSPI + LVGL: our shipping display stack (same as cyd-35r).
#include <TFT_eSPI.h>
#include <lvgl.h>

// S3-only bring-up libraries (registry-resolved; mirrored into
// copilot-setup-steps.yml pre-warm; never vendored into lib/).
#define XPOWERS_CHIP_AXP2101          // select the AXP2101 in XPowersLib
#include "XPowersLib.h"              // AXP2101 PMIC (I2C 0x34)
#include "TCA9554.h"                // TCA9554 IO expander (I2C 0x20) - LCD reset
#include "TouchDrvFT6X36.hpp"       // FT6336 capacitive touch (I2C 0x38)

// ---------------------------------------------------------------------------
// Pin map - Waveshare ESP32-S3-Touch-LCD-3.5 (non-B), from issue #120.
// Display SPI pins are also fed to TFT_eSPI via build flags in platformio.ini
// (TFT_MOSI/TFT_SCLK/TFT_DC/...); they are repeated here only for the log.
// ---------------------------------------------------------------------------
static constexpr int PIN_I2C_SDA  = 8;   // shared I2C bus
static constexpr int PIN_I2C_SCL  = 7;
static constexpr int PIN_LCD_MOSI = 1;   // (informational; TFT_eSPI owns these)
static constexpr int PIN_LCD_SCLK = 5;
static constexpr int PIN_LCD_DC   = 3;
static constexpr int PIN_LCD_BL   = 6;   // backlight enable

// I2C device addresses (7-bit).
static constexpr uint8_t ADDR_AXP2101 = 0x34;
static constexpr uint8_t ADDR_TCA9554 = 0x20;
static constexpr uint8_t ADDR_FT6336  = 0x38;

// TCA9554 expander pin that drives the LCD reset line.
static constexpr uint8_t TCA_LCD_RESET_PIN = 1;

// Landscape geometry (rotation 1). Native ST7796 panel is 320x480 portrait;
// TFT_WIDTH/TFT_HEIGHT build flags stay portrait and rotation makes it 480x320.
static constexpr int SCREEN_W       = 480;
static constexpr int SCREEN_H       = 320;
static constexpr int DRAW_BUF_LINES = 40;  // partial buffer; S3 has RAM to spare
static constexpr uint8_t SCREEN_ROTATION = 1;

// Firmware SHA/version - injected by CI via -D, "dev" otherwise (same pattern
// as the other firmwares; purely cosmetic in the banner).
#ifndef FW_GIT_SHA
#  define FW_GIT_SHA ""
#endif
#ifndef FW_VERSION
#  define FW_VERSION ""
#endif
static inline const char* fw_git_sha() {
    return (FW_GIT_SHA[0] != '\0') ? FW_GIT_SHA : "dev";
}
static inline const char* fw_version() {
    return (FW_VERSION[0] != '\0') ? FW_VERSION : "dev";
}

// ---------------------------------------------------------------------------
// Hardware objects.
// ---------------------------------------------------------------------------
static TFT_eSPI       tft;
static XPowersPMU     pmu;
static TCA9554        tca(ADDR_TCA9554);
static TouchDrvFT6X36 touch;

// LVGL 9 partial draw buffer. RGB565 = 2 bytes/px. alignas(64) satisfies LVGL
// 9's LV_DRAW_BUF_ALIGN assertion in lv_display_set_buffers() (a bare array can
// silently hang). Same pattern as src/diag/diag_main.cpp.
alignas(64) static uint8_t draw_buf[SCREEN_W * DRAW_BUF_LINES * 2];

static bool g_display_ok = false;
static bool g_touch_ok   = false;

// ---------------------------------------------------------------------------
// LVGL flush - identical code path to the production/diag CYD firmware, just on
// the S3's ST7796. TFT_eSPI handles the RGB565 byte order to match lv_conf.h.
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

// ---------------------------------------------------------------------------
// (b) I2C bus.
// ---------------------------------------------------------------------------
static void bring_up_i2c() {
    Serial.printf("[i2c ] begin SDA=IO%d SCL=IO%d ... ", PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    Serial.println("[ OK ]");
}

// Print the live AXP2101 fuel-gauge / power state. Called once after init and
// then periodically from loop() so a bench operator can watch charge behaviour.
static void print_pmu_status() {
    Serial.println("[pmu ] ---- AXP2101 status ----");
    Serial.printf("[pmu ]   battery present : %s\n", pmu.isBatteryConnect() ? "yes" : "no");
    Serial.printf("[pmu ]   charging        : %s\n", pmu.isCharging() ? "yes" : "no");
    Serial.printf("[pmu ]   VBUS present    : %s\n", pmu.isVbusIn() ? "yes" : "no");
    Serial.printf("[pmu ]   battery SOC     : %d %%\n", pmu.getBatteryPercent());
    Serial.printf("[pmu ]   battery voltage : %u mV\n", pmu.getBattVoltage());
    Serial.printf("[pmu ]   VBUS voltage    : %u mV\n", pmu.getVbusVoltage());
    Serial.printf("[pmu ]   system voltage  : %u mV\n", pmu.getSystemVoltage());
    Serial.println("[pmu ] ------------------------");
}

// ---------------------------------------------------------------------------
// (c) AXP2101 - MUST come first: a rail left off can keep the display/touch
// dark. We mirror the vendor 02_axp2101_example rail set (it enables essentially
// every rail); which rail actually powers the backlight-enable and the touch is
// an open question for the bench (issue #120) - once known, this can be trimmed.
// ---------------------------------------------------------------------------
static bool bring_up_pmic() {
    Serial.print("[pmu ] AXP2101 begin @0x34 ... ");
    if (!pmu.begin(Wire, ADDR_AXP2101, PIN_I2C_SDA, PIN_I2C_SCL)) {
        Serial.println("[FAIL] PMIC not responding - panel/touch will stay dark");
        return false;
    }
    Serial.printf("[ OK ] chipID=0x%x\n", pmu.getChipID());

    // Input protection (vendor demo values).
    pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
    pmu.setSysPowerDownVoltage(2600);

    // Rail voltages (vendor demo). Kept verbatim so a rail that the panel needs
    // is not accidentally under-volted during bring-up.
    pmu.setDC2Voltage(1000);
    pmu.setDC3Voltage(3300);
    pmu.setDC4Voltage(1000);
    pmu.setDC5Voltage(3300);
    pmu.setALDO1Voltage(3300);
    pmu.setALDO2Voltage(3300);
    pmu.setALDO3Voltage(3300);
    pmu.setALDO4Voltage(3300);
    pmu.setBLDO1Voltage(1500);
    pmu.setBLDO2Voltage(2800);
    pmu.setCPUSLDOVoltage(1000);
    pmu.setDLDO1Voltage(3300);
    pmu.setDLDO2Voltage(3300);

    // Enable the rails the vendor demo enables (DC1 left off - it feeds the SoC
    // core and is managed by the PMU boot default).
    pmu.enableDC2();
    pmu.enableDC3();
    pmu.enableDC4();
    pmu.enableDC5();
    pmu.enableALDO1();
    pmu.enableALDO2();
    pmu.enableALDO3();
    pmu.enableALDO4();
    pmu.enableBLDO1();
    pmu.enableBLDO2();
    pmu.enableCPUSLDO();
    pmu.enableDLDO1();
    pmu.enableDLDO2();

    // This board has no battery-temperature (TS) thermistor. Leaving TS measure
    // on makes the PMU think the battery is over/under temperature and refuse to
    // charge, so the vendor demo disables it explicitly.
    pmu.disableTSPinMeasure();

    // Turn on the ADC paths that back the fuel-gauge readouts.
    pmu.enableBattDetection();
    pmu.enableVbusVoltageMeasure();
    pmu.enableBattVoltageMeasure();
    pmu.enableSystemVoltageMeasure();

    // Charge LED off - noise on the bench, not needed for the spike.
    pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);

    // Let the ADCs settle before the first readout.
    delay(50);
    print_pmu_status();
    return true;
}

// ---------------------------------------------------------------------------
// (d) LCD reset via TCA9554 pin 1. On this board the ST7796 reset line is NOT a
// GPIO - it hangs off the TCA9554 expander, so it must be pulsed here (in
// software) BEFORE tft.init(). Sequence + timing from the vendor lcd_reset():
// high -> low(10ms) -> high(200ms).
// ---------------------------------------------------------------------------
static void lcd_reset_via_expander() {
    Serial.print("[tca ] TCA9554 begin @0x20 ... ");
    if (!tca.begin()) {
        // Non-fatal for the log, but the display almost certainly will not come
        // up without a real reset pulse - flag it loudly.
        Serial.println("[FAIL] expander not responding - LCD reset will be a no-op");
    } else {
        Serial.println("[ OK ]");
    }
    tca.pinMode1(TCA_LCD_RESET_PIN, OUTPUT);

    Serial.print("[tca ] pulsing LCD reset (pin 1: high->low->high) ... ");
    tca.write1(TCA_LCD_RESET_PIN, HIGH);
    delay(10);
    tca.write1(TCA_LCD_RESET_PIN, LOW);
    delay(10);
    tca.write1(TCA_LCD_RESET_PIN, HIGH);
    delay(200);
    Serial.println("[done]");
}

// ---------------------------------------------------------------------------
// (e) ST7796 over TFT_eSPI + LVGL 9 smoke screen.
// ---------------------------------------------------------------------------
static void draw_lvgl_smoke_screen() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);

    // A filled rounded rectangle - proves fills + geometry render.
    lv_obj_t* card = lv_obj_create(scr);
    lv_obj_set_size(card, 320, 150);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1e6f5c), LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);

    // Text - proves the font path (LOAD_* fonts) renders through LVGL.
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "S3 bring-up OK");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t* sub = lv_label_create(card);
    lv_label_set_text_fmt(sub, "ST7796 @80MHz  %dx%d\nfw %s (%s)\nTap to test FT6336 touch",
                          SCREEN_W, SCREEN_H, fw_version(), fw_git_sha());
    lv_obj_set_style_text_color(sub, lv_color_hex(0xd0f0e8), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void bring_up_display() {
    Serial.print("[lcd ] TFT_eSPI/ST7796 init ... ");
    tft.init();
    tft.setRotation(SCREEN_ROTATION);
    tft.fillScreen(TFT_BLACK);
    Serial.println("[ OK ]");

    // Backlight enable. Full-on is enough to prove the rail/pin for the spike;
    // production will drive IO6 with LEDC PWM (5 kHz) per the #120 pin map.
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);
    Serial.printf("[lcd ] backlight IO%d -> HIGH\n", PIN_LCD_BL);

    Serial.print("[lvgl] init + smoke screen ... ");
    lv_init();
    lv_display_t* disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    draw_lvgl_smoke_screen();
    lv_timer_handler();   // force a first render so the panel shows immediately
    Serial.println("[ OK ]");
    g_display_ok = true;
}

// ---------------------------------------------------------------------------
// (f) FT6336 capacitive touch. INT/RST are not wired on this board, so we poll.
// ---------------------------------------------------------------------------
static bool bring_up_touch() {
    Serial.print("[tch ] FT6336 begin @0x38 ... ");
    if (!touch.begin(Wire, ADDR_FT6336, PIN_I2C_SDA, PIN_I2C_SCL)) {
        Serial.println("[FAIL] FT6336 not responding - check the shared I2C bus");
        return false;
    }
    // Match the panel geometry so reported coordinates line up with what is
    // drawn. Rotation/mirror vs our landscape is an open bench question (#120).
    touch.setMaxCoordinates(SCREEN_W, SCREEN_H);
    Serial.println("[ OK ]");
    return true;
}

// ---------------------------------------------------------------------------
// Poll one touch point and log transitions (press / release). Kept dead simple:
// this is a bring-up probe, not the production indev driver.
// ---------------------------------------------------------------------------
static void poll_touch() {
    static bool was_down = false;
    int16_t x[1] = {0};
    int16_t y[1] = {0};
    const uint8_t n = touch.getPoint(x, y, 1);
    if (n > 0) {
        if (!was_down) {
            Serial.printf("[tch ] touch DOWN at (%d, %d)\n", x[0], y[0]);
            was_down = true;
        }
    } else if (was_down) {
        Serial.println("[tch ] touch UP");
        was_down = false;
    }
}

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
    Serial.begin(115200);
    // USB-CDC: wait briefly for the host to attach so the banner is not lost.
    const unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 3000) {
        delay(10);
    }

    Serial.println();
    Serial.println("===========================================================");
    Serial.println(" OpenSprinkler Panel - S3 bring-up spike (issue #140)");
    Serial.println(" Board: Waveshare ESP32-S3-Touch-LCD-3.5 (non-B)");
    Serial.printf(" Firmware: %s (%s)\n", fw_version(), fw_git_sha());
    Serial.println(" Stages: I2C -> AXP2101 -> TCA9554 reset -> ST7796/LVGL -> FT6336");
    Serial.println("===========================================================");

    bring_up_i2c();                 // (b)
    const bool pmic_ok = bring_up_pmic();   // (c) - rails first
    lcd_reset_via_expander();       // (d) - reset before display init
    bring_up_display();             // (e)
    g_touch_ok = bring_up_touch();  // (f)

    Serial.println("-----------------------------------------------------------");
    Serial.println(" Bring-up summary:");
    Serial.printf("   AXP2101 PMIC  : %s\n", pmic_ok      ? "OK"   : "FAIL");
    Serial.printf("   ST7796 display: %s\n", g_display_ok ? "OK"   : "FAIL");
    Serial.printf("   FT6336 touch  : %s\n", g_touch_ok   ? "OK"   : "FAIL");
    Serial.println(" Watch the panel for the smoke screen; tap to log touches.");
    Serial.println(" PMIC status reprints every 5 s below.");
    Serial.println("-----------------------------------------------------------");
}

// ===========================================================================
// loop()
// ===========================================================================
void loop() {
    static uint32_t last_tick_ms = millis();
    static uint32_t last_pmu_ms  = millis();

    // Drive LVGL.
    const uint32_t now = millis();
    lv_tick_inc(now - last_tick_ms);
    last_tick_ms = now;
    lv_timer_handler();

    // Poll capacitive touch (if it came up).
    if (g_touch_ok) {
        poll_touch();
    }

    // Periodically reprint the fuel-gauge state so charge/plug behaviour is
    // observable on the bench over a charge/discharge cycle.
    if (now - last_pmu_ms >= 5000) {
        last_pmu_ms = now;
        print_pmu_status();
    }

    delay(5);
}
