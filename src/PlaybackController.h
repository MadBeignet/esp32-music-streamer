#pragma once

#include <Arduino.h>

#include "AlbumArt.h"
#include "AudioEngine.h"
#include "AudioModule.h"
#include "BrowserUtils.h"
#include "FileBrowser.h"
#include "NowPlaying.h"
#include "PinConfig.h"

extern AudioEngine audio;

// Drives the actual playback lifecycle: which track is playing, whether
// it's paused, and what happens when a track finishes (repeat/shuffle/
// advance) or is stopped manually. FileBrowser owns *which* item is
// selected in the list; this module owns *what's currently playing*.
namespace Playback {
  enum class Mode {
    Normal,
    RepeatAll,
    RepeatOne,
    Shuffle
  };

  static Mode mode = Mode::Normal;
  static int currentTrackIndex = -1;
  static bool active = false;  // true while the Now Playing screen is shown
  static bool paused = false;

  inline bool isActive() { return active; }
  inline bool isPaused() { return paused; }

  // Starts playing the track at `trackIndex` in the browser's current item
  // list: updates playback/title state, clears any leftover album art, and
  // hands the file path to the audio engine.
  inline void startTrackAtIndex(int trackIndex) {
    if (!FileBrowser::isPlayableItem(trackIndex)) return;

    FileBrowser::currentSelection = trackIndex;
    currentTrackIndex = trackIndex;
    NowPlaying::resetPendingTitle();

    String fullPath = BrowserUtils::buildFullPath(
        FileBrowser::currentPath, FileBrowser::menuItems[trackIndex]);
    NowPlaying::setTitle(
        BrowserUtils::displayTitleFromFilename(FileBrowser::menuItems[trackIndex]));

    // The TFT and SD card share one SPI bus; make sure the TFT is
    // deselected before playback starts issuing SD reads.
    digitalWrite(TFT_TCS, HIGH);
    digitalWrite(TFT_CCS, HIGH);
    delay(10);

    AlbumArt::clear();
    active = true;
    paused = false;
    NowPlaying::render();
    AudioPlayer::play(fullPath.c_str());
  }

  inline void startSelectedTrack() {
    startTrackAtIndex(FileBrowser::currentSelection);
  }

  inline void togglePause() {
    paused = !paused;
    audio.pauseResume();
    NowPlaying::render();
  }

  inline void seekForwardSeconds(uint32_t seconds) {
    uint32_t curTime = audio.getAudioCurrentTime();
    audio.setAudioPlayPosition(curTime + seconds);
  }

  // Stops playback and returns to the file browser (used by both the
  // "stop"/"back" touch controls and a fully-finished track with nothing
  // left to advance to).
  inline void stopAndReturnToBrowser() {
    audio.stopSong();
    active = false;
    paused = false;
    AlbumArt::clear();
    currentTrackIndex = -1;
    FileBrowser::readDirectory(FileBrowser::currentPath);
  }

  // Called once a track finishes naturally to decide and start whatever
  // should play next, honoring the current play mode. Falls back to
  // stopping and returning to the browser if there's nothing left to play.
  inline void continueToNextTrack() {
    if (!active || currentTrackIndex < 0) return;

    int nextTrack = -1;
    if (mode == Mode::RepeatOne) {
      nextTrack = currentTrackIndex;
    } else {
      nextTrack = FileBrowser::findNextPlayableIndex(currentTrackIndex);
      if (nextTrack < 0 && mode == Mode::RepeatAll) {
        for (int candidate = 0; candidate < FileBrowser::totalItems; ++candidate) {
          if (FileBrowser::isPlayableItem(candidate)) {
            nextTrack = candidate;
            break;
          }
        }
      }
    }

    if (nextTrack >= 0) {
      startTrackAtIndex(nextTrack);
    } else {
      stopAndReturnToBrowser();
    }
  }
}
