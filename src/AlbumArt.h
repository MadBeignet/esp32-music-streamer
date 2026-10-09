#pragma once

#include <Arduino.h>
#include <freertos/semphr.h>

// Holds the currently-decoded embedded cover image (JPEG/PNG bytes handed to
// us by the audio_id3image() library callback) in a PSRAM buffer. Written
// from the audio decode task, read from the main UI loop, so access is
// guarded by a mutex.
namespace AlbumArt {
  static SemaphoreHandle_t mutex = nullptr;
  static uint8_t* data = nullptr;
  static size_t size = 0;
  static bool available = false;
  static bool dirty = false;  // set when a new cover arrives; UI should redraw

  inline void lock() {
    if (mutex == nullptr) {
      mutex = xSemaphoreCreateMutex();
    }
    if (mutex != nullptr) {
      xSemaphoreTake(mutex, portMAX_DELAY);
    }
  }

  inline void unlock() {
    if (mutex != nullptr) {
      xSemaphoreGive(mutex);
    }
  }

  // Drops the current cover (if any). Called both when starting a fresh
  // track (old cover no longer applies) and when stopping playback.
  inline void clear() {
    lock();
    if (data != nullptr) {
      free(data);
      data = nullptr;
    }
    size = 0;
    available = false;
    dirty = false;
    unlock();
  }

  // Called from the audio_id3image() library callback when a new embedded
  // cover image has been decoded. Copies the bytes into a fresh PSRAM
  // buffer so they outlive the library's own temporary buffer.
  inline void store(const uint8_t* imageBuffer, size_t imageSize) {
    lock();
    if (data != nullptr) {
      free(data);
      data = nullptr;
    }
    size = 0;
    available = false;

    data = static_cast<uint8_t*>(ps_malloc(imageSize));
    if (data != nullptr) {
      memcpy(data, imageBuffer, imageSize);
      size = imageSize;
      available = true;
      dirty = true;
      Serial.printf("Artwork stored in PSRAM: %u bytes\n",
                    static_cast<unsigned>(size));
    } else {
      Serial.printf("Artwork allocation failed: %u bytes\n",
                    static_cast<unsigned>(imageSize));
    }
    unlock();
  }

  // Returns true (and clears the flag) exactly once per new cover arrival.
  inline bool consumeDirty() {
    if (!dirty) return false;
    dirty = false;
    return true;
  }
}
