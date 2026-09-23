#pragma once

#include <Arduino.h>

namespace AppConfig {
  constexpr int MaxMenuItems = 64;
  constexpr int BrowserVisibleItems = 8;
  constexpr uint32_t TouchDebounceMs = 60;
  constexpr uint8_t RequiredStableTouchSamples = 2;

  constexpr uint16_t TouchUp = _BV(0);
  constexpr uint16_t TouchDown = _BV(1);
  constexpr uint16_t TouchSelect = _BV(2);
  constexpr uint16_t TouchBack = _BV(3);
  constexpr uint16_t TouchSeekForward = _BV(4);
  constexpr uint16_t TouchStop = _BV(5);
  constexpr uint16_t TouchVolumeDown = _BV(6);
  constexpr uint16_t TouchVolumeUp = _BV(7);
}
