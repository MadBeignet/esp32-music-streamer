#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPR121.h>

#include "AppConfig.h"
#include "PinConfig.h"

// Reads the MPR121 capacitive touch pads and turns raw readings into clean,
// debounced "newly pressed" button events (each physical press reported
// exactly once, not repeatedly while held down).
namespace TouchInput {
  static TwoWire touchWire = TwoWire(1);
  static Adafruit_MPR121 sensor;

  static uint16_t lastState = 0;
  static uint16_t pendingState = 0;
  static uint8_t pendingSamples = 0;
  static uint32_t lastEventMs = 0;

  inline void begin() {
    Serial.println("Initializing MPR121...");
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, INPUT_PULLUP);

    Wire.begin(I2C_SDA, I2C_SCL, 100000);
    delay(100);

    touchWire.begin(I2C_SDA, I2C_SCL, 100000);
    delay(100);

    if (!sensor.begin(0x5A, &touchWire)) {
      Serial.println("MPR121 not found on D0/D1!");
    } else {
      Serial.println("MPR121 touch engine online!");
      sensor.setThresholds(6, 3);   // Sensitive thresholds (touch: 6, release: 3)
      sensor.setAutoconfig(true);   // Let the library manage baseline/filtering
    }
  }

  // Polls the sensor and, if a new stable button press is detected, writes
  // its bitmask into `newlyPressed` and returns true. Requires the reading
  // to stay stable for AppConfig::RequiredStableTouchSamples consecutive
  // polls before accepting it (debounce), plus a minimum gap between
  // accepted events.
  inline bool poll(uint16_t& newlyPressed) {
    uint16_t touchedState = sensor.touched();

    if (touchedState == 0xFFFF) {
      Serial.println("Failed to read: 0xFFFF");
      return false;
    }

    if (touchedState != pendingState) {
      pendingState = touchedState;
      pendingSamples = 1;
      return false;
    }
    if (pendingSamples < AppConfig::RequiredStableTouchSamples) {
      pendingSamples++;
      return false;
    }

    newlyPressed = touchedState & ~lastState;
    lastState = touchedState;

    if (newlyPressed == 0 || millis() - lastEventMs < AppConfig::TouchDebounceMs) {
      return false;
    }
    lastEventMs = millis();
    return true;
  }
}
