#pragma once

#include <Arduino.h>
#include "AudioEngine.h"
#include <SdFat.h>

extern AudioEngine audio;
extern SdFat sd; 

namespace AudioPlayer {
  static uint8_t currentVolume = 12; 
  static uint8_t currentEqMode = 1;  

  enum EQPreset {
    EQ_FLAT = 0,
    EQ_BASS_BOOST,
    EQ_VOCAL,
    EQ_WARM,
    EQ_COUNT
  };

  inline void applyPreset(EQPreset preset) {
    switch (preset) {
      case EQ_BASS_BOOST: break;
      case EQ_VOCAL:      break;
      case EQ_WARM:       break;
      case EQ_FLAT:
      default:            break;
    }
  }

  inline const char* getPresetName(int presetIndex) {
    switch (presetIndex) {
      case EQ_FLAT:       return "Flat (Off)";
      case EQ_BASS_BOOST: return "Bass Boost";
      case EQ_VOCAL:      return "Vocal Clarity";
      case EQ_WARM:       return "Warm Profile";
      default:            return "Unknown";
    }
  }

  inline uint8_t getCurrentEqMode() { return currentEqMode; }

  inline void setEQMode(uint8_t mode) {
    if (mode >= EQ_COUNT) mode = 0;
    currentEqMode = mode;
    applyPreset((EQPreset)currentEqMode);
  }

  inline void init() {
    delay(50);
    audio.begin(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume((int)map(currentVolume, 0, 21, 0, 100));
    applyPreset((EQPreset)currentEqMode);
  }

  inline void play(const char* filepath) {
    audio.playPath(filepath);
  }

  inline void tick() {
    audio.loop();
  }

  inline bool consumeTrackFinished() {
    return audio.consumeTrackFinished();
  }

  inline uint8_t getVolume() { return currentVolume; }

  inline void setVolume(uint8_t vol) {
    if (vol > 21) vol = 21;
    currentVolume = vol;
    audio.setVolume((int)map(currentVolume, 0, 21, 0, 100));
  }

  inline void volumeUp() {
    if (currentVolume < 21) {
      currentVolume++;
      setVolume((int)currentVolume);
    }
  }

  inline void volumeDown() {
    if (currentVolume > 0) {
      currentVolume--;
      setVolume((int)currentVolume);
    }
  }
}