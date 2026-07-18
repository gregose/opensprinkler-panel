#include <Arduino.h>

// M0 — Toolchain bring-up: serial hello + RGB-LED heartbeat.
// On the E32R35T / ESP32-3248S035R the RGB LED is common-anode (LOW = on) on
// GPIO22/16/17, and the LCD backlight is on GPIO27 (HIGH = on).
// See docs/03-architecture.md for the full pin map.
static const int PIN_LED_R = 22;
static const int PIN_LED_G = 16;
static const int PIN_LED_B = 17;
static const int PIN_BACKLIGHT = 27;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("OpenSprinkler panel — M0 toolchain OK");

  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_BACKLIGHT, OUTPUT);

  digitalWrite(PIN_LED_R, HIGH);      // off (common-anode)
  digitalWrite(PIN_LED_G, HIGH);      // off
  digitalWrite(PIN_LED_B, HIGH);      // off
  digitalWrite(PIN_BACKLIGHT, HIGH);  // backlight on
}

void loop() {
  digitalWrite(PIN_LED_G, LOW);       // green on
  Serial.println("heartbeat");
  delay(500);
  digitalWrite(PIN_LED_G, HIGH);      // green off
  delay(500);
}
