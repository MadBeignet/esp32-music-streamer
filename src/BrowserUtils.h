#pragma once

#include <Arduino.h>

namespace BrowserUtils {

inline int extractLeadingNumber(const String& name) {
  int number = 0;
  bool foundDigit = false;
  for (size_t i = 0; i < name.length(); ++i) {
    if (isdigit(name[i])) {
      number = number * 10 + (name[i] - '0');
      foundDigit = true;
    } else if (foundDigit) {
      break;
    }
  }
  return foundDigit ? number : 999999;
}

inline bool isSupportedAudio(const String& filename) {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".mp3") ||
         lower.endsWith(".wav") ||
         lower.endsWith(".flac") ||
         lower.endsWith(".m4a") ||
         lower.endsWith(".aac") ||
         lower.endsWith(".ogg") ||
         lower.endsWith(".opus");
}

inline String displayTitleFromFilename(const String& filename) {
  String title = filename;
  const int extensionStart = title.lastIndexOf('.');
  if (extensionStart > 0) {
    title.remove(extensionStart);
  }

  int firstNonDigit = 0;
  while (firstNonDigit < title.length() &&
         isdigit(title[firstNonDigit])) {
    ++firstNonDigit;
  }
  if (firstNonDigit > 0 && firstNonDigit < title.length() &&
      title[firstNonDigit] == ' ') {
    title = title.substring(firstNonDigit + 1);
  }
  title.trim();
  return title;
}

inline String buildFullPath(const String& path, const String& filename) {
  if (path == "/") return "/" + filename;
  if (path.endsWith("/")) return path + filename;
  return path + "/" + filename;
}

inline bool navigateUpDirectory(String& path) {
  if (path == "/" || path == "") {
    path = "/";
    return false;
  }

  if (path.endsWith("/") && path.length() > 1) {
    path.remove(path.length() - 1);
  }

  int lastSlash = path.lastIndexOf('/');
  if (lastSlash <= 0) {
    path = "/";
  } else {
    path = path.substring(0, lastSlash);
  }
  return true;
}

inline void sortDirectoryItems(String items[], bool isFolder[], int itemCount) {
  for (int i = 0; i < itemCount - 1; ++i) {
    for (int j = 0; j < itemCount - i - 1; ++j) {
      bool swapNeeded = false;
      if (!isFolder[j] && isFolder[j + 1]) {
        swapNeeded = true;
      } else if (isFolder[j] == isFolder[j + 1]) {
        int numberA = extractLeadingNumber(items[j]);
        int numberB = extractLeadingNumber(items[j + 1]);
        swapNeeded = numberA > numberB ||
                     (numberA == numberB &&
                      items[j].compareTo(items[j + 1]) > 0);
      }

      if (swapNeeded) {
        String name = items[j];
        items[j] = items[j + 1];
        items[j + 1] = name;

        bool folder = isFolder[j];
        isFolder[j] = isFolder[j + 1];
        isFolder[j + 1] = folder;
      }
    }
  }
}

}  // namespace BrowserUtils
