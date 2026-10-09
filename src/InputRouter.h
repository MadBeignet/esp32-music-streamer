#pragma once

#include <Arduino.h>

#include "AppConfig.h"
#include "AudioModule.h"
#include "DisplayModule.h"
#include "FileBrowser.h"
#include "NowPlaying.h"
#include "PlaybackController.h"

// Decides what a touch event means based on which screen is currently
// showing. FileBrowser owns its own touch handling directly (it only ever
// needs its own state); this module covers the EQ menu and the Now
// Playing screen's playback controls, then dispatches every incoming
// touch event to whichever of the three is active.
namespace InputRouter {
  static bool eqMenuActive = false;
  static int eqMenuSelection = 0;

  inline void handleEqMenuTouch(uint16_t newlyPressed) {
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
      eqMenuActive = false;
      NowPlaying::render();
    }
  }

  inline void handlePlayingScreenTouch(uint16_t newlyPressed) {
    if (newlyPressed & AppConfig::TouchDown) {
      eqMenuActive = true;
      eqMenuSelection = AudioPlayer::getCurrentEqMode();
      Display::drawEQMenu(eqMenuSelection, AudioPlayer::EQ_COUNT, AudioPlayer::getPresetName);
    } else if (newlyPressed & AppConfig::TouchStop) {
      Playback::stopAndReturnToBrowser();
    } else if (newlyPressed & AppConfig::TouchUp) {
      Playback::togglePause();
    } else if (newlyPressed & AppConfig::TouchBack) {
      Playback::stopAndReturnToBrowser();
    } else if (newlyPressed & AppConfig::TouchSeekForward) {
      Playback::seekForwardSeconds(5);
    } else if (newlyPressed & AppConfig::TouchVolumeDown) {
      AudioPlayer::volumeDown();
      NowPlaying::render();
    } else if (newlyPressed & AppConfig::TouchVolumeUp) {
      AudioPlayer::volumeUp();
      NowPlaying::render();
    }
  }

  // The single entry point: called once per loop() iteration with each new
  // touch event. Routes it to the EQ menu, the Now Playing screen, or the
  // file browser, depending on what's currently displayed.
  inline void handleTouch(uint16_t newlyPressed) {
    Serial.printf("Touch detected! Newly pressed mask: 0x%04X\n", newlyPressed);

    if (eqMenuActive) {
      handleEqMenuTouch(newlyPressed);
    } else if (Playback::isActive()) {
      handlePlayingScreenTouch(newlyPressed);
    } else {
      FileBrowser::handleTouch(newlyPressed);
    }
  }
}
