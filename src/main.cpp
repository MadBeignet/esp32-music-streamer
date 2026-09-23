#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MPR121.h>
#include <SdFat.h>

#include "DisplayModule.h"            

#include "AudioEngine.h"
#include "AudioModule.h"
#include "AppConfig.h"
#include "BrowserUtils.h"

LGFX_XiaoS3_ST7735 tft;
SdFat sd;
AudioEngine audio;

TaskHandle_t AudioTaskHandle = NULL;

TwoWire CustomWire = TwoWire(1); 
Adafruit_MPR121 touchServer = Adafruit_MPR121();

String currentPath = "/";
String menuItems[AppConfig::MaxMenuItems];
bool itemIsFolder[AppConfig::MaxMenuItems];
int totalItems = 0;
int currentSelection = 0;
int browserFirstVisible = 0;
int currentTrackIndex = -1;

enum class PlayMode {
  Normal,
  RepeatAll,
  RepeatOne,
  Shuffle
};

PlayMode playMode = PlayMode::Normal;

bool musicModeActive = false;
bool eqMenuModeActive = false;
bool hasAlbumCover = false;
int eqMenuSelection = 0;

bool isPaused = false;
uint16_t lastTouchedState = 0;
uint16_t pendingTouchedState = 0;
uint8_t pendingTouchSamples = 0;
uint32_t lastTouchEventMs = 0;

String currentSongTitle = "";
String currentArtist = "";

void AudioTask(void *pvParameters) {
  Serial.printf("Audio task started on core %d\n", xPortGetCoreID());
  for (;;) {
    AudioPlayer::tick();
    vTaskDelay(1);
  }
}

void renderPlayingScreen() {
  const String& title = currentSongTitle.length() > 0
                            ? currentSongTitle
                            : BrowserUtils::displayTitleFromFilename(
                                menuItems[currentSelection]);
  Display::showPlayingScreen(
    title.c_str(),
    isPaused,
    AudioPlayer::getVolume(),
    AudioPlayer::getPresetName(AudioPlayer::getCurrentEqMode()),
    hasAlbumCover
  );
}

void redrawPlayingScreen() {
  renderPlayingScreen();
}

bool isPlayableItem(int index) {
  return index >= 0 && index < totalItems && !itemIsFolder[index];
}

bool readDirectory(const String& path) {
    Serial.printf("Reading directory: %s\n", path.c_str());
    FsFile root = sd.open(path.c_str(), O_RDONLY);
    if (!root || !root.isDir()) {
        Serial.printf("Failed to open directory: %s\n", path.c_str());
        return false;
    }

    String newItems[AppConfig::MaxMenuItems];
    bool newItemIsFolder[AppConfig::MaxMenuItems];
    int newTotalItems = 0;
    FsFile entry;
    while (entry.openNext(&root, O_RDONLY)) {
        char nameBuf[64];
        entry.getName(nameBuf, sizeof(nameBuf));
        
        String nameStr = String(nameBuf);

        if (nameStr.startsWith(".")) {
            entry.close();
            continue;
        }

        if (entry.isDir()) {
            if (newTotalItems < AppConfig::MaxMenuItems) {
                newItems[newTotalItems] = nameStr;
                newItemIsFolder[newTotalItems] = true;
                newTotalItems++;
            }
        } else if (BrowserUtils::isSupportedAudio(nameStr)) {
            if (newTotalItems < AppConfig::MaxMenuItems) {
                newItems[newTotalItems] = nameStr;
                newItemIsFolder[newTotalItems] = false;
                newTotalItems++;
            }
        }
        entry.close();
    }
    root.close();

    BrowserUtils::sortDirectoryItems(newItems, newItemIsFolder, newTotalItems);
    for (int i = 0; i < newTotalItems; ++i) {
        menuItems[i] = newItems[i];
        itemIsFolder[i] = newItemIsFolder[i];
    }
    totalItems = newTotalItems;
    currentSelection = 0;
    browserFirstVisible = 0;

    Display::drawBrowser(path.c_str(), menuItems, itemIsFolder, totalItems,
                         currentSelection, browserFirstVisible);
    return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n--- BOOT TEST: STAGE 0 (BASIC SERIAL) ---");

  if (psramFound()) {
    Serial.printf("PSRAM Free: %d bytes\n", ESP.getFreePsram());
  }

  Serial.println("Initializing Display...");
  Display::init();

  Serial.println("Initializing SD Card...");
  pinMode(TFT_TCS, OUTPUT);
  pinMode(TFT_CCS, OUTPUT);
  digitalWrite(TFT_TCS, HIGH);
  digitalWrite(TFT_CCS, HIGH);
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CCS);
  if (!sd.begin(TFT_CCS, SD_SCK_MHZ(20))) {
    Serial.println("SD Card Fault!");
  } else {
    Serial.println("SD Card mounted!");
  }

  Serial.println("Initializing Audio...");
  AudioPlayer::init();

  Serial.println("Initializing MPR121...");
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL, 100000); 
  delay(100); 

  CustomWire.begin(I2C_SDA, I2C_SCL, 100000); 
  delay(100); 

  if (!touchServer.begin(0x5A, &CustomWire)) {
    Serial.println("MPR121 not found on D0/D1!");
  } else {
    Serial.println("MPR121 touch engine online!");
    touchServer.setThresholds(6, 3); // Sensitive thresholds (touch: 6, release: 3)
    touchServer.setAutoconfig(true); // Let the library manage baseline and filtering automatically
  }

  Serial.println("Reading Directory...");
  readDirectory(currentPath);

  Serial.println("Starting Audio Task...");
  xTaskCreatePinnedToCore(
    AudioTask,
    "AudioTask",
    32768,
    NULL,
    1,
    &AudioTaskHandle,
    0
  );

  Serial.println("--- BOOT TEST COMPLETED STAGE 0 CLEANLY ---");
}

void stopPlaybackAndReturnToBrowser() {
  audio.stopSong();
  musicModeActive = false;
  isPaused = false;
  hasAlbumCover = false;
  currentTrackIndex = -1;
  readDirectory(currentPath);
}

void handleEqMenuTouch(uint16_t newlyPressed) {
  if (newlyPressed & AppConfig::TouchUp) {
    eqMenuSelection = (eqMenuSelection > 0) ? eqMenuSelection - 1 : AudioPlayer::EQ_COUNT - 1;
    Display::drawEQMenu(eqMenuSelection, AudioPlayer::EQ_COUNT, AudioPlayer::getPresetName);
  } else if (newlyPressed & AppConfig::TouchDown) {
    eqMenuSelection = (eqMenuSelection < AudioPlayer::EQ_COUNT - 1) ? eqMenuSelection + 1 : 0;
    Display::drawEQMenu(eqMenuSelection, AudioPlayer::EQ_COUNT, AudioPlayer::getPresetName);
  } else if (newlyPressed & AppConfig::TouchSelect) {
    AudioPlayer::setEQMode(eqMenuSelection);
    Display::drawEQMenu(eqMenuSelection, AudioPlayer::EQ_COUNT, AudioPlayer::getPresetName);
  } else if (newlyPressed & AppConfig::TouchBack) {
    eqMenuModeActive = false;
    redrawPlayingScreen();
  }
}

void handleMusicTouch(uint16_t newlyPressed) {
  if (newlyPressed & AppConfig::TouchDown) {
    eqMenuModeActive = true;
    eqMenuSelection = AudioPlayer::getCurrentEqMode();
    Display::drawEQMenu(eqMenuSelection, AudioPlayer::EQ_COUNT, AudioPlayer::getPresetName);
  } else if (newlyPressed & AppConfig::TouchStop) {
    stopPlaybackAndReturnToBrowser();
  } else if (newlyPressed & AppConfig::TouchUp) {
    isPaused = !isPaused;
    audio.pauseResume();
    redrawPlayingScreen();
  } else if (newlyPressed & AppConfig::TouchBack) {
    stopPlaybackAndReturnToBrowser();
  } else if (newlyPressed & AppConfig::TouchSeekForward) {
    uint32_t curTime = audio.getAudioCurrentTime();
    audio.setAudioPlayPosition(curTime + 5);
  } else if (newlyPressed & AppConfig::TouchVolumeDown) {
    AudioPlayer::volumeDown();
    redrawPlayingScreen();
  } else if (newlyPressed & AppConfig::TouchVolumeUp) {
    AudioPlayer::volumeUp();
    redrawPlayingScreen();
  }
}

void startTrackAtIndex(int trackIndex) {
  if (!isPlayableItem(trackIndex)) return;
  currentSelection = trackIndex;
  currentTrackIndex = trackIndex;
  String fullPath = BrowserUtils::buildFullPath(currentPath, menuItems[trackIndex]);
  currentSongTitle = BrowserUtils::displayTitleFromFilename(
      menuItems[trackIndex]);
  char safeAudioPath[256];
  strncpy(safeAudioPath, fullPath.c_str(), sizeof(safeAudioPath));
  safeAudioPath[sizeof(safeAudioPath) - 1] = '\0';

  digitalWrite(TFT_TCS, HIGH);
  digitalWrite(TFT_CCS, HIGH);
  delay(10);

  hasAlbumCover = false;
  AudioPlayer::play(safeAudioPath);
  musicModeActive = true;
  isPaused = false;

  redrawPlayingScreen();
}

void startSelectedAudio() {
  startTrackAtIndex(currentSelection);
}

int findNextTrackIndex(int index) {
  for (int candidate = index + 1; candidate < totalItems; ++candidate) {
    if (isPlayableItem(candidate)) return candidate;
  }
  return -1;
}

void continueToNextTrack() {
  if (!musicModeActive || currentTrackIndex < 0) return;

  int nextTrack = -1;
  if (playMode == PlayMode::RepeatOne) {
    nextTrack = currentTrackIndex;
  } else {
    nextTrack = findNextTrackIndex(currentTrackIndex);
    if (nextTrack < 0 && playMode == PlayMode::RepeatAll) {
      for (int candidate = 0; candidate < totalItems; ++candidate) {
        if (isPlayableItem(candidate)) {
          nextTrack = candidate;
          break;
        }
      }
    }
  }

  if (nextTrack >= 0) {
    startTrackAtIndex(nextTrack);
  } else {
    stopPlaybackAndReturnToBrowser();
  }
}

void handleBrowserTouch(uint16_t newlyPressed) {
  if (totalItems == 0 && !(newlyPressed & AppConfig::TouchBack)) {
    return;
  }

  if (newlyPressed & AppConfig::TouchUp) {
    const bool wrapped = currentSelection == 0;
    currentSelection = wrapped ? totalItems - 1 : currentSelection - 1;
    if (wrapped) {
      browserFirstVisible = max(0, totalItems - AppConfig::BrowserVisibleItems);
    }
    if (currentSelection < browserFirstVisible) {
      browserFirstVisible = currentSelection;
    }
    Display::drawBrowser(currentPath.c_str(), menuItems, itemIsFolder, totalItems,
                         currentSelection, browserFirstVisible);
  } else if (newlyPressed & AppConfig::TouchDown) {
    const bool wrapped = currentSelection == totalItems - 1;
    currentSelection = wrapped ? 0 : currentSelection + 1;
    if (wrapped) {
      browserFirstVisible = 0;
    }
    const int lastVisible = browserFirstVisible + AppConfig::BrowserVisibleItems - 1;
    if (currentSelection > lastVisible) {
      browserFirstVisible = currentSelection - AppConfig::BrowserVisibleItems + 1;
    }
    Display::drawBrowser(currentPath.c_str(), menuItems, itemIsFolder, totalItems,
                         currentSelection, browserFirstVisible);
  } else if (newlyPressed & AppConfig::TouchBack) {
    String parentPath = currentPath;
    if (BrowserUtils::navigateUpDirectory(parentPath)) {
      if (readDirectory(parentPath)) {
        currentPath = parentPath;
      }
    } else {
      Serial.println("Already at root directory");
      Display::drawBrowser(currentPath.c_str(), menuItems, itemIsFolder, totalItems,
                           currentSelection, browserFirstVisible);
    }
  } else if (newlyPressed & AppConfig::TouchSelect) {
    if (itemIsFolder[currentSelection]) {
      String childPath = BrowserUtils::buildFullPath(
          currentPath, menuItems[currentSelection]);
      if (readDirectory(childPath)) {
        currentPath = childPath;
      }
    } else {
      startSelectedAudio();
    }
  }
}

bool readNewTouch(uint16_t &newlyPressed) {
  uint16_t touchedState = touchServer.touched();

  if (touchedState == 0xFFFF) {
    Serial.println("Failed to read: 0xFFFF");
    return false;
  } 

  if (touchedState != pendingTouchedState) {
    pendingTouchedState = touchedState;
    pendingTouchSamples = 1;
    return false;
  }
  if (pendingTouchSamples < AppConfig::RequiredStableTouchSamples) {
    pendingTouchSamples++;
    return false;
  }

  newlyPressed = touchedState & ~lastTouchedState;
  lastTouchedState = touchedState;

  if (newlyPressed == 0 ||
      millis() - lastTouchEventMs < AppConfig::TouchDebounceMs) {
    return false;
  }
  lastTouchEventMs = millis();
  return true;
}

void handleTouch(uint16_t newlyPressed) {
  Serial.printf("Touch detected! Newly pressed mask: 0x%04X\n", newlyPressed);

  if (eqMenuModeActive) {
    handleEqMenuTouch(newlyPressed);
    return;
  }

  if (musicModeActive) {
    handleMusicTouch(newlyPressed);
    return;
  }

  handleBrowserTouch(newlyPressed);
}

void loop() {
  if (AudioPlayer::consumeTrackFinished()) {
    continueToNextTrack();
  }

  uint16_t newlyPressed = 0;
  if (readNewTouch(newlyPressed)) {
    handleTouch(newlyPressed);
  }
  vTaskDelay(1);
}

// Library Callback: Fires automatically when an ID3 embedded cover image is decoded
void audio_id3image(const char *info, const uint8_t *imageBuffer, size_t imageSize) {
  // Display writes stay on the UI task; decoder callbacks run on AudioTask.
  if (musicModeActive && imageBuffer && imageSize > 0) hasAlbumCover = true;
}

void audio_metadata(audio_tools::MetaDataType type, const char* value, int length) {
  if (type != audio_tools::Title || value == nullptr || length <= 0) return;
  currentSongTitle = String(value).substring(0, length);
  currentSongTitle.trim();
}

void audio_info(const char *info){ Serial.println(info); }