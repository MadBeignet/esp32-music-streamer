#pragma once

#include <Arduino.h>
#include <SdFat.h>

#include "AppConfig.h"
#include "BrowserUtils.h"
#include "DisplayModule.h"

extern SdFat sd;

// Forward declaration only (no #include): FileBrowser needs to *start* a
// track once one is selected, but PlaybackController.h needs the browser's
// item list, so it fully includes this header instead. Declaring just the
// one function we call avoids a circular include.
namespace Playback {
  void startTrackAtIndex(int trackIndex);
}

// Owns the on-screen directory listing: the current folder path, its
// contents, and which entry is highlighted. Also handles touch input while
// the browser screen is showing (move selection, enter a folder, go back,
// or start playback of the selected track).
namespace FileBrowser {
  static String currentPath = "/";
  static String menuItems[AppConfig::MaxMenuItems];
  static bool itemIsFolder[AppConfig::MaxMenuItems];
  static int totalItems = 0;
  static int currentSelection = 0;
  static int firstVisible = 0;

  inline bool isPlayableItem(int index) {
    return index >= 0 && index < totalItems && !itemIsFolder[index];
  }

  // Finds the next playable (non-folder) entry strictly after `index`, or
  // -1 if there isn't one.
  inline int findNextPlayableIndex(int index) {
    for (int candidate = index + 1; candidate < totalItems; ++candidate) {
      if (isPlayableItem(candidate)) return candidate;
    }
    return -1;
  }

  inline void redraw() {
    Display::drawBrowser(currentPath.c_str(), menuItems, itemIsFolder,
                         totalItems, currentSelection, firstVisible);
  }

  // Reads `path`'s directory contents into the item list (folders first,
  // then tracks, both alphabetically/numerically sorted), resets the
  // selection, and redraws the browser screen. Returns false if the
  // directory couldn't be opened.
  inline bool readDirectory(const String& path) {
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
      char nameBuf[768];
      const size_t nameLength = entry.getName(nameBuf, sizeof(nameBuf));
      if (nameLength == 0) {
        Serial.println("Skipping directory entry with unreadable/oversized name");
        entry.close();
        continue;
      }

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
    firstVisible = 0;

    redraw();
    return true;
  }

  inline void handleTouch(uint16_t newlyPressed) {
    if (totalItems == 0 && !(newlyPressed & AppConfig::TouchBack)) {
      return;
    }

    if (newlyPressed & AppConfig::TouchUp) {
      const bool wrapped = currentSelection == 0;
      currentSelection = wrapped ? totalItems - 1 : currentSelection - 1;
      if (wrapped) {
        firstVisible = max(0, totalItems - AppConfig::BrowserVisibleItems);
      }
      if (currentSelection < firstVisible) {
        firstVisible = currentSelection;
      }
      redraw();
    } else if (newlyPressed & AppConfig::TouchDown) {
      const bool wrapped = currentSelection == totalItems - 1;
      currentSelection = wrapped ? 0 : currentSelection + 1;
      if (wrapped) {
        firstVisible = 0;
      }
      const int lastVisible = firstVisible + AppConfig::BrowserVisibleItems - 1;
      if (currentSelection > lastVisible) {
        firstVisible = currentSelection - AppConfig::BrowserVisibleItems + 1;
      }
      redraw();
    } else if (newlyPressed & AppConfig::TouchBack) {
      String parentPath = currentPath;
      if (BrowserUtils::navigateUpDirectory(parentPath)) {
        if (readDirectory(parentPath)) {
          currentPath = parentPath;
        }
      } else {
        Serial.println("Already at root directory");
        redraw();
      }
    } else if (newlyPressed & AppConfig::TouchSelect) {
      if (itemIsFolder[currentSelection]) {
        String childPath = BrowserUtils::buildFullPath(currentPath, menuItems[currentSelection]);
        if (readDirectory(childPath)) {
          currentPath = childPath;
        }
      } else {
        Playback::startTrackAtIndex(currentSelection);
      }
    }
  }

  // Reads the root directory so the browser has something to show at boot.
  inline void begin() {
    Serial.println("Reading Directory...");
    readDirectory(currentPath);
  }
}
