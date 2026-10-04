// ESP32-S3 SD-card music player.
//
// High-level flow (see each module's own header for the details):
//   1. setup() brings up hardware in dependency order - display, SD card,
//      audio engine, touch sensor - reads the root directory so the
//      browser has something to show, then starts the background audio
//      pipeline task.
//   2. loop() (Core 1) drives the UI every ~1ms: checks whether the
//      current track finished, applies any pending track-title/album-art
//      updates, polls touch input and routes it, and steps the SD-read
//      half of the audio pipeline.
//   3. AudioTask (Core 0, high priority) continuously drains
//      already-decoded PCM to I2S. FLAC/etc. decode compute runs in
//      AudioEngine's own dedicated, lower-priority Core 0 task, so it can
//      never delay AudioTask (see AudioEngine.h for that split in detail).
//
// Module map - click into any of these to see how that piece works:
//   FileBrowser.h         - directory listing/navigation + its own touch input
//   PlaybackController.h  - what track is playing, pause/seek, track-advance
//   NowPlaying.h          - the "Now Playing" screen + track title state
//   AlbumArt.h            - embedded cover art storage
//   TouchInput.h          - MPR121 driver + debounced button events
//   InputRouter.h         - routes touch events to whichever screen is active
//   AudioEngine.h / AudioModule.h - the decode/drain pipeline itself
//      (SD read -> FLAC/MP3/AAC/WAV decode -> I2S output)

#include <Arduino.h>
#include <SdFat.h>

#include "DisplayModule.h"
#include "AudioEngine.h"
#include "AudioModule.h"
#include "PinConfig.h"

#include "AlbumArt.h"
#include "FileBrowser.h"
#include "NowPlaying.h"
#include "PlaybackController.h"
#include "TouchInput.h"
#include "InputRouter.h"

// Shared hardware handles used across modules.
SemaphoreHandle_t sharedSpiMutex = nullptr;
LGFX_XiaoS3_ST7735 tft;
SdFat sd;
AudioEngine audio;

TaskHandle_t AudioTaskHandle = NULL;

// Dedicated Core 0 task: drains the already-decoded PCM ring to I2S. Runs
// at a higher priority than everything else on Core 0 (see AudioEngine's
// separate, lower-priority decode task) so it is always preempted back in
// promptly and can keep the DMA fed at a steady, glitch-free rate. SD reads
// happen separately on Core 1, inside loop() below; FLAC/etc. decode
// compute happens in AudioEngine's own dedicated Core 0 task.
void AudioTask(void *pvParameters) {
  Serial.printf("Audio drain task started on core %d\n", xPortGetCoreID());
  for (;;) {
    AudioPlayer::tick();
    vTaskDelay(1);
  }
}

static void initSdCard() {
  Serial.println("Initializing SD Card...");
  pinMode(TFT_TCS, OUTPUT);
  pinMode(TFT_CCS, OUTPUT);
  digitalWrite(TFT_TCS, HIGH);
  digitalWrite(TFT_CCS, HIGH);
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CCS);
  // 40MHz caused SD init/reads to fail on this card/wiring (blank screen -
  // the directory read after sd.begin() never completed), so back to the
  // conservative, known-working 20MHz. This is independent of the TFT's own
  // SPI clock (DisplayModule.h's cfg.freq_write, still 20MHz) - each
  // device's driver sets its own clock per transaction even though they
  // share the same physical bus/pins.
  if (!sd.begin(TFT_CCS, SD_SCK_MHZ(20))) {
    Serial.println("SD Card Fault!");
  } else {
    Serial.println("SD Card mounted!");
  }
}

static void startAudioTask() {
  Serial.println("Starting Audio Task...");
  xTaskCreatePinnedToCore(
    AudioTask,
    "AudioTask",
    32768,
    NULL,
    2,
    &AudioTaskHandle,
    0
  );
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- BOOT TEST: STAGE 0 (BASIC SERIAL) ---");

  sharedSpiMutex = xSemaphoreCreateMutex();
  if (sharedSpiMutex == nullptr) {
    Serial.println("Shared SPI mutex allocation failed!");
    return;
  }

  if (psramFound()) {
    Serial.printf("PSRAM Free: %d bytes\n", ESP.getFreePsram());
  }

  Serial.println("Initializing Display...");
  Display::init();

  initSdCard();

  Serial.println("Initializing Audio...");
  AudioPlayer::init();

  TouchInput::begin();
  FileBrowser::begin();
  startAudioTask();

  Serial.println("--- BOOT TEST COMPLETED STAGE 0 CLEANLY ---");
}

void loop() {
  if (AudioPlayer::consumeTrackFinished()) {
    Playback::continueToNextTrack();
  }

  NowPlaying::pollMetadataUpdate();
  NowPlaying::pollAlbumArtUpdate();

  uint16_t newlyPressed = 0;
  if (TouchInput::poll(newlyPressed)) {
    InputRouter::handleTouch(newlyPressed);
  }

  // SD reads run cooperatively on this core (Core 1, alongside touch/UI
  // work) instead of a dedicated task, so they naturally yield to touch
  // handling and screen redraws above. FLAC/etc. decode compute happens in
  // AudioEngine's own dedicated, low-priority Core 0 task; AudioTask
  // (Core 0, high priority) only drains the already-decoded PCM ring to
  // I2S, so it never blocks on SD/decode work.
  audio.decodeStep();
  vTaskDelay(1);
}

// ---------------------------------------------------------------------
// arduino-audio-tools library callbacks. These must keep this exact global
// function name/signature - the library calls them directly by symbol,
// not through any of our own namespaces.
// ---------------------------------------------------------------------

// Fires when an ID3/FLAC-embedded cover image has been decoded.
void audio_id3image(const char *info, const uint8_t *imageBuffer, size_t imageSize) {
  if (!Playback::isActive() || imageBuffer == nullptr || imageSize == 0) {
    Serial.printf("Artwork ignored: active=%d size=%u\n",
                  Playback::isActive(), static_cast<unsigned>(imageSize));
    return;
  }
  Serial.printf("Artwork callback: %s, %u bytes\n", info,
                static_cast<unsigned>(imageSize));
  AlbumArt::store(imageBuffer, imageSize);
}

// Fires when a track's metadata (e.g. title tag) becomes available.
void audio_metadata(audio_tools::MetaDataType type, const char* value, int length) {
  if (type != audio_tools::Title || value == nullptr || length <= 0) return;
  NowPlaying::onMetadataTitle(value, length);
}
