// Board HAL — CYD (ESP32-3248S035R, "cheap yellow display") implementation.
//
// Holds the CYD-specific display / touch / battery / reset code that used to
// live inline in src/main.cpp, moved here behind the board_* seams declared in
// board.h. Behaviour is intended to be identical to the pre-HAL main.cpp.
//
// Hardware: ST7796U display (480x320 landscape) + XPT2046 resistive touch on a
// shared SPI bus, BAT sense on GPIO34 (VBAT/2 through a 100K/100K divider). See
// docs/03 for the pin map. Compiled only when the CYD board is selected.
#if defined(OSP_BOARD_CYD)

#include "board.h"

#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

// The display object is owned by main.cpp (it is also used by the pre-LVGL boot
// screens and the LVGL flush callback, which stay in main.cpp). We share it here
// so the board seams drive the same instance.
extern TFT_eSPI tft;

namespace {

// Touch-calibration persistence. These MUST match main.cpp's NVS_NS /
// NVS_TOUCHCAL and the diag firmware: a diag-seeded calibration is honored by
// production and vice-versa, and main.cpp's factory reset clears this same key.
constexpr const char* kNvsNamespace = "osp-panel";
constexpr const char* kNvsTouchCal  = "touch_cal";

// Battery sense: BAT_ADC on GPIO34 (ADC1_CH6, input-only, Wi-Fi-safe) reads
// VBAT/2 through an even 100K/100K divider (schematic E32R35T, ratio confirmed
// 1.991 on-device). See lib/battery_monitor.
constexpr int PIN_BAT_ADC     = 34;
// Battery: multisample count per read. The gauge is coarse and EMA-smoothed, so
// a slow cadence (owned by the caller) keeps it calm and cheap.
constexpr int BAT_ADC_SAMPLES = 16;

// Load a saved calData[5] blob from NVS and apply it via setTouch().
// Returns true if a valid 10-byte calibration was found and applied.
// NVS namespace/key matches the diag firmware so a diag-seeded calibration
// is honored by production and vice-versa.
bool load_touch_cal() {
    uint16_t calData[5] = {0};
    Preferences prefs;
    prefs.begin(kNvsNamespace, true);
    const size_t got = prefs.getBytes(kNvsTouchCal, calData, sizeof(calData));
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
// draws directly via TFT_eSPI and must not fight an active LVGL display. The
// caller shows the "calibration saved" notice (app boot chrome).
void run_touch_calibration() {
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
    prefs.begin(kNvsNamespace, false);
    prefs.putBytes(kNvsTouchCal, calData, sizeof(calData));
    prefs.end();
    Serial.println("Touch: calibration complete and saved to NVS.");
}

}  // namespace

void board_reset_panel() {
    // CYD wires TFT_RST to -1 (no software reset line) — nothing to do.
}

void board_display_init() {
    tft.init();
    // LVGL 9 renders RGB565 in the ESP32's native little-endian byte order, but
    // tft.pushPixels() sends 16-bit words MSB-first — so LVGL buffers must be
    // byte-swapped or colours come out wrong (near-black bg renders red-ish,
    // anti-aliased text edges turn to rainbow speckle). setSwapBytes only affects
    // buffer pushes (pushPixels/pushImage), NOT fillScreen/drawString, so raw
    // graphics are unaffected. Must be set before any LVGL flush.
    tft.setSwapBytes(true);
    tft.setRotation(1);  // landscape — tune rotation/offset on-device (docs/03)
}

bool board_touch_init() {
    if (!load_touch_cal()) {
        run_touch_calibration();
        return true;
    }
    return false;
}

void board_touch_read(uint16_t& x, uint16_t& y, bool& pressed) {
    pressed = tft.getTouch(&x, &y);
}

int board_battery_read() {
    // Battery sense ADC: ~0–3.1 V range so VBAT/2 (1.5–2.1 V) sits in the
    // linear region. Uses the eFuse Vref calibration via analogReadMilliVolts.
    // Attenuation only needs to be set once.
    static bool atten_set = false;
    if (!atten_set) {
        analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
        atten_set = true;
    }
    uint32_t sum = 0;
    for (int i = 0; i < BAT_ADC_SAMPLES; ++i) {
        sum += analogReadMilliVolts(PIN_BAT_ADC);
    }
    return static_cast<int>(sum / BAT_ADC_SAMPLES);
}

bool board_external_power() {
    return false;  // no charger-status telemetry on this board
}

#endif  // OSP_BOARD_CYD
