# ESP32-S3 Music Streamer

An SD-card music player built around an ESP32-S3. The project provides a
touch-controlled browser for music stored on the SD card, decodes supported
audio formats, and outputs PCM audio over I2S to an external DAC.

This is an active embedded project. Some hardware details, enclosure details,
and user-interface behavior are still being refined.

Overall goals are being able to decode a wide variety of audio types that 
aren't common in typical players while still sounding decent.

## Current capabilities

- Browse folders and audio files on an SD card.
- Sort directory entries naturally, including numbered tracks.
- Scroll through directory contents with the touch controls.
- Start playback from the selected track while retaining the surrounding
  tracks in the same folder for continuation.
- Automatically continue to the next track when a song reaches EOF.
- Support for normal playback, repeat-one, repeat-all, and planned shuffle
  playback modes. The mode-selection UI is still being developed.
- Audio playback through an I2S DAC.
- Supported or integrated decoder paths for:
  - MP3
  - WAV
  - FLAC
  - AAC/M4A
  - OGG/Opus integration points
- AAC/M4A handling for common AAC-LC, 44.1 kHz, stereo files.
- Fast-start detection and a temporary rewrite path for M4A files whose
  metadata appears after the media data.
- Album-art handling where supported by the source file and display path.
- Touch-controlled pause, stop, seek, volume, and equalizer controls.
- Runtime memory diagnostics for internal heap and PSRAM usage.
- Native regression tests for MP4/M4A layout and classification logic.

## Hardware

### Main controller

- Seeed Xiao ESP32-S3 development board
- 8 MB OPI PSRAM
- PlatformIO with the Arduino-ESP32 framework

### Audio

- I2S digital audio output
- External PCM5102-style I2S DAC
- Current I2S connections:
  - BCLK: GPIO 44
  - LRCK/WS: GPIO 43
  - DATA: GPIO 6

### Storage

- SD card connected over SPI
- `SdFat` library
- Current SPI connections:
  - MOSI: GPIO 9
  - MISO: GPIO 8
  - SCK: GPIO 7
  - SD chip select: GPIO 5

### Display

- 1.8-inch-class ST7735 SPI TFT display
- `LovyanGFX` library
- Current display connections:
  - Data/command: GPIO 3
  - Chip select: GPIO 4
  - Reset: tied to 3.3 V / not controlled by a GPIO

### Touch input

- MPR121 capacitive touch controller
- Software I2C bus
- Current connections:
  - SDA: GPIO 1
  - SCL: GPIO 2
- The current firmware maps electrodes to navigation, selection, playback,
  seeking, and volume controls.

> Hardware board names, display dimensions, DAC wiring details, touch-pad
> layout, and enclosure information can be expanded here as the build is
> finalized.

## Software architecture

- `src/main.cpp` contains application state, touch handling, directory
  navigation, and playlist continuation.
- `src/AudioEngine.h` owns the decoder chain, SD streaming, I2S output, audio
  task, PSRAM-backed buffers, and M4A handling.
- `src/AudioModule.h` provides the application-facing audio wrapper.
- `src/DisplayModule.h` contains display initialization and rendering.
- `src/AppConfig.h` contains UI and touch constants.
- `src/PinConfig.h` contains board pin assignments.
- Audio processing runs in a dedicated FreeRTOS task to keep decoding separate
  from the UI loop.

## Building and testing

Install PlatformIO, then run:

```bash
pio run -e seeed_xiao_esp32s3
pio test -e native
```

The native tests do not require the ESP32 hardware. Local audio fixtures are
used for some regression checks when available; they are not required for the
firmware build.

## Project status

The project is under active development. Known areas still being refined
include shuffle and playback-mode controls, broader M4A compatibility,
high-resolution FLAC performance, and hardware-level regression testing across
different SD cards and audio files.
