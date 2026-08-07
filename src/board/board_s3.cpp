// Board HAL — Waveshare ESP32-S3-Touch-LCD-3.5 (non-B) implementation.
//
// The S3 sibling to board_cyd.cpp: the same board_* seams from board.h, but
// backed by the S3's very different chipset (ST7796 on a dedicated SPI3 bus,
// FT6336 capacitive touch over I2C, AXP2101 PMIC, and a TCA9554 IO expander that
// owns the LCD reset line). Every choreography below is lifted verbatim from the
// bench-proven bring-up spike (s3diag/diag_s3_main.cpp, PR0 #141) so production
// matches what was validated on real hardware. Compiled only when the S3 board
// is selected (-D OSP_BOARD_S3), so it is an empty translation unit for the CYD.
//
// Hardware: ESP32-S3R8 (8 MB PSRAM, 16 MB flash). Shared I2C bus on SDA IO8 /
// SCL IO7 carries the PMIC (0x34), the reset expander (0x20), and the touch
// controller (0x38). See issue #120 for the confirmed pin map.
#if defined(OSP_BOARD_S3)

#include "board.h"

#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>

#include "panel_state.h"

// S3-only libraries (registry-resolved; pre-warmed in copilot-setup-steps.yml;
// never vendored into lib/). Same set + versions as the s3-touch-35-diag env.
#define XPOWERS_CHIP_AXP2101       // select the AXP2101 variant in XPowersLib
#include "XPowersLib.h"           // AXP2101 PMIC (I2C 0x34)
#include "TCA9554.h"             // TCA9554 IO expander (I2C 0x20) — LCD reset
#include "TouchDrvFT6X36.hpp"    // FT6336 capacitive touch (I2C 0x38)

// The display object is owned by main.cpp (shared with the boot screens + the
// LVGL flush callback). We drive the same instance here, exactly like board_cyd.
extern TFT_eSPI tft;

namespace {

// Shared I2C bus pins (PMIC, expander, touch all hang off this one bus).
constexpr int PIN_I2C_SDA = 8;
constexpr int PIN_I2C_SCL = 7;

// I2C device addresses (7-bit).
constexpr uint8_t ADDR_AXP2101 = 0x34;
constexpr uint8_t ADDR_TCA9554 = 0x20;
constexpr uint8_t ADDR_FT6336  = 0x38;

// TCA9554 expander pin wired to the ST7796 reset line.
constexpr uint8_t TCA_LCD_RESET_PIN = 1;

// Landscape display geometry (rotation 1). The ST7796 panel is 320x480 native
// portrait; TFT_WIDTH/TFT_HEIGHT stay portrait and setRotation(1) makes it
// 480x320. The FT6336, however, always reports in NATIVE portrait coordinates,
// so board_touch_read() maps them into this display space.
constexpr int SCREEN_W        = 480;
constexpr int SCREEN_H        = 320;
constexpr uint8_t SCREEN_ROTATION = 1;

// FT6336 native portrait bounds (width x height) used both to bound the driver
// and to scale/rotate reported points into landscape display space.
constexpr int TOUCH_NATIVE_W = 320;   // native X range 0..319
constexpr int TOUCH_NATIVE_H = 480;   // native Y range 0..479

// Shared hardware objects, brought up in board_reset_panel() and reused by the
// touch/battery seams. File-static so they live for the whole program.
XPowersPMU     pmu;
TCA9554        tca(ADDR_TCA9554);
TouchDrvFT6X36 touch;

bool g_touch_ok = false;

// (c) AXP2101 — MUST come up before the display: several rails power the panel
// and the touch controller, so a rail left off keeps them dark. Rail set mirrors
// the vendor 02_axp2101_example (kept verbatim from the spike so a rail the panel
// needs is never under-volted). Charge/plug state is intentionally NOT surfaced
// here — that is a later PR (#136/#118).
void bring_up_pmic() {
    if (!pmu.begin(Wire, ADDR_AXP2101, PIN_I2C_SDA, PIN_I2C_SCL)) {
        Serial.println("[board_s3] AXP2101 not responding — panel/touch may stay dark");
        return;
    }

    pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
    pmu.setSysPowerDownVoltage(2600);

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

    // No battery-temperature thermistor on this board — leaving TS measure on
    // makes the PMU refuse to charge, so disable it (vendor demo does the same).
    pmu.disableTSPinMeasure();

    // ADC paths that back the fuel-gauge readouts used by board_battery_read().
    pmu.enableBattDetection();
    pmu.enableVbusVoltageMeasure();
    pmu.enableBattVoltageMeasure();
    pmu.enableSystemVoltageMeasure();

    pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
    delay(50);  // let the ADCs settle before the first readout
}

// (d) Pulse the ST7796 reset line through the TCA9554. On this board reset is not
// a GPIO — it hangs off the expander — so it must be driven in software BEFORE
// tft.init(). Sequence + timing from the vendor lcd_reset(): high->low(10ms)->
// high(200ms).
void lcd_reset_via_expander() {
    if (!tca.begin()) {
        Serial.println("[board_s3] TCA9554 not responding — LCD reset is a no-op");
    }
    tca.pinMode1(TCA_LCD_RESET_PIN, OUTPUT);
    tca.write1(TCA_LCD_RESET_PIN, HIGH);
    delay(10);
    tca.write1(TCA_LCD_RESET_PIN, LOW);
    delay(10);
    tca.write1(TCA_LCD_RESET_PIN, HIGH);
    delay(200);
}

}  // namespace

void board_reset_panel() {
    // Bring up the shared I2C bus, then the PMIC rails, then pulse the LCD reset
    // — all of which must happen before board_display_init() drives the ST7796.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    bring_up_pmic();
    lcd_reset_via_expander();
}

void board_display_init() {
    // NOTE: TFT_eSPI logs "spiAttachMISO(): HSPI Does not have default pins on
    // ESP32S3!" — benign: MISO is unused on this write-only panel (TFT_MISO=-1).
    tft.init();
    // LVGL 9 renders RGB565 little-endian; tft.pushPixels() sends 16-bit words
    // MSB-first, so LVGL buffers must be byte-swapped or colours shift. Same fix
    // as the CYD (setSwapBytes only affects buffer pushes, not fillScreen/text).
    tft.setSwapBytes(true);
    // This board's ST7796 needs display inversion ON (INVON) or colours come out
    // complemented (dark bg -> white). This is the OPPOSITE of the CYD's panel,
    // which wants invert OFF — a per-panel difference proven on the bench (#141).
    tft.invertDisplay(true);
    tft.setRotation(SCREEN_ROTATION);  // landscape 480x320
    // Backlight (IO6) is driven by main.cpp's LEDC PWM path (shared with the
    // sleep-dimming logic), same as the CYD — nothing to do here.
}

bool board_touch_init() {
    // FT6336 capacitive touch. INT/RST are not wired, so the caller polls.
    g_touch_ok = touch.begin(Wire, ADDR_FT6336, PIN_I2C_SDA, PIN_I2C_SCL);
    if (!g_touch_ok) {
        Serial.println("[board_s3] FT6336 not responding — check the shared I2C bus");
    }
    // Bound the driver to the native portrait geometry so getPoint() reports the
    // full native range (board_touch_read maps it into landscape display space).
    touch.setMaxCoordinates(TOUCH_NATIVE_W, TOUCH_NATIVE_H);
    // Capacitive touch needs no calibration, so there is never a "calibration
    // saved" boot notice to show — always return false.
    return false;
}

void board_touch_read(uint16_t& x, uint16_t& y, bool& pressed) {
    if (!g_touch_ok) {
        pressed = false;
        return;
    }

    int16_t nx = 0;   // native portrait X (0..TOUCH_NATIVE_W-1)
    int16_t ny = 0;   // native portrait Y (0..TOUCH_NATIVE_H-1)
    const uint8_t n = touch.getPoint(&nx, &ny, 1);
    if (n == 0) {
        pressed = false;
        return;
    }

    // Native-portrait -> landscape (rotation 1) mapping, characterized on the
    // bench (#141). Reported native X tracks the physical VERTICAL axis inverted
    // (top of the landscape panel reads high native-X), and native Y tracks the
    // physical HORIZONTAL axis (left reads low native-Y). So:
    //   displayX = native Y
    //   displayY = (native width - 1) - native X   (invert)
    // Center check: native (160,240) -> display (240,159), i.e. panel centre.
    int dx = ny;
    int dy = (TOUCH_NATIVE_W - 1) - nx;

    if (dx < 0) dx = 0;
    if (dx > SCREEN_W - 1) dx = SCREEN_W - 1;
    if (dy < 0) dy = 0;
    if (dy > SCREEN_H - 1) dy = SCREEN_H - 1;

    x = static_cast<uint16_t>(dx);
    y = static_cast<uint16_t>(dy);
    pressed = true;
}

int board_battery_read() {
    // board.h contract: return "millivolts at the sense tap", which the caller
    // (BatteryMonitor) multiplies by kBatteryDividerRatio (2.0) to recover VBAT
    // before mapping to a LiPo %-of-charge curve. The AXP2101 reports TRUE VBAT
    // directly (no divider), so we return VBAT/2 here: the caller's x2 then
    // recovers the real VBAT and the SAME curve produces the same gauge as the
    // CYD. Keeps board.h and the gauge behaviour unchanged.
    return static_cast<int>(pmu.getBattVoltage()) / 2;
}

bool board_external_power() {
    return pmu.isVbusIn();
}

void board_status_led_set(osp::StatusLed state) {
    switch (state) {
        case osp::StatusLed::Off:
            pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
            break;
        case osp::StatusLed::On:
            pmu.setChargingLedMode(XPOWERS_CHG_LED_ON);
            break;
        case osp::StatusLed::SlowBlink:
            pmu.setChargingLedMode(XPOWERS_CHG_LED_BLINK_1HZ);
            break;
        case osp::StatusLed::FastBlink:
            pmu.setChargingLedMode(XPOWERS_CHG_LED_BLINK_4HZ);
            break;
    }
}

#endif  // OSP_BOARD_S3
