#pragma once

#include <Arduino.h>

#include "AlbumArt.h"
#include "AudioModule.h"
#include "BrowserUtils.h"
#include "DisplayModule.h"
#include "FileBrowser.h"

// Forward declarations only (no #include): render() needs to know whether
// playback is paused/active, but PlaybackController.h needs this header
// (for render()/onMetadataTitle()), so it can't be included back here.
namespace Playback {
  bool isPaused();
  bool isActive();
}

// Owns what's shown on the "Now Playing" screen: the current track's
// display title (from its own metadata tag if available, otherwise its
// filename) plus the logic to redraw that screen. Track metadata tags
// arrive asynchronously from the decode task, so a pending-title handoff
// is guarded by a critical section.
namespace NowPlaying {
  static String title = "";
  static portMUX_TYPE metadataMux = portMUX_INITIALIZER_UNLOCKED;
  static char pendingTitle[128] = {};
  static bool pendingTitleReady = false;

  inline void setTitle(const String& newTitle) {
    title = newTitle;
  }

  // Clears any metadata title left over from a previous track. Call this
  // right before starting a new one.
  inline void resetPendingTitle() {
    portENTER_CRITICAL(&metadataMux);
    pendingTitle[0] = '\0';
    pendingTitleReady = false;
    portEXIT_CRITICAL(&metadataMux);
  }

  // Called from the audio_metadata() library callback (runs on the decode
  // task) whenever the current file's embedded title tag becomes
  // available. Only stashes the value; the main loop picks it up via
  // pollMetadataUpdate() so the screen is only ever redrawn from loop().
  inline void onMetadataTitle(const char* value, int length) {
    const size_t copyLength = min(static_cast<size_t>(length), sizeof(pendingTitle) - 1);
    portENTER_CRITICAL(&metadataMux);
    memcpy(pendingTitle, value, copyLength);
    pendingTitle[copyLength] = '\0';
    pendingTitleReady = true;
    portEXIT_CRITICAL(&metadataMux);
  }

  inline void render() {
    const String& displayTitle =
        title.length() > 0
            ? title
            : BrowserUtils::displayTitleFromFilename(
                  FileBrowser::menuItems[FileBrowser::currentSelection]);

    AlbumArt::lock();
    const bool coverReady =
        AlbumArt::available && AlbumArt::data != nullptr && AlbumArt::size > 0;
    Display::showPlayingScreen(
        displayTitle.c_str(),
        Playback::isPaused(),
        AudioPlayer::getVolume(),
        AudioPlayer::getPresetName(AudioPlayer::getCurrentEqMode()),
        coverReady,
        AlbumArt::data,
        AlbumArt::size);
    AlbumArt::unlock();
  }

  // Applies any pending metadata title update and redraws the screen if
  // the title actually changed. Call once per loop() iteration.
  inline void pollMetadataUpdate() {
    char nextTitle[sizeof(pendingTitle)];
    bool changed = false;
    portENTER_CRITICAL(&metadataMux);
    if (pendingTitleReady) {
      memcpy(nextTitle, pendingTitle, sizeof(nextTitle));
      pendingTitleReady = false;
      changed = true;
    }
    portEXIT_CRITICAL(&metadataMux);
    if (changed) {
      title = nextTitle;
      if (Playback::isActive()) render();
    }
  }

  // Redraws the screen once if new album art has arrived since the last
  // check. Call once per loop() iteration.
  inline void pollAlbumArtUpdate() {
    if (Playback::isActive() && AlbumArt::consumeDirty()) {
      render();
    }
  }
}
