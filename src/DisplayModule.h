#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "PinConfig.h"
#include "AppConfig.h"
#include "BrowserUtils.h"
#include <freertos/semphr.h>

extern SemaphoreHandle_t sharedSpiMutex;

class LGFX_XiaoS3_ST7735 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7735S _panel_instance;
  lgfx::Bus_SPI       _bus_instance;

public:
  LGFX_XiaoS3_ST7735() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;     
      cfg.spi_mode = 0;             
      cfg.freq_write = 20000000;    
      cfg.pin_sclk = TFT_SCK;             
      cfg.pin_mosi = TFT_MOSI;             
      cfg.pin_miso = TFT_MISO;             
      cfg.pin_dc   = TFT_TDC;             
      
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = TFT_TCS;     
      cfg.pin_rst          = TFT_RST;    
      cfg.panel_width      = 128;   
      cfg.panel_height     = 160;   
      cfg.memory_width     = 128;   
      cfg.memory_height    = 160;   
      cfg.bus_shared       = true; 
      // LGFX defaults rgb_order to false (BGR), but this panel's controller
      // needs RGB order - without this, every color drawn (not just album
      // art) has red and blue channels swapped. This is a panel-wiring
      // setting, unrelated to JPEG/PNG decoding.
      cfg.rgb_order        = true;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

extern LGFX_XiaoS3_ST7735 tft;

namespace Display {
  class SpiBusLock {
  public:
    SpiBusLock() {
      if (sharedSpiMutex != nullptr) {
        xSemaphoreTake(sharedSpiMutex, portMAX_DELAY);
        locked = true;
      }
    }

    ~SpiBusLock() {
      if (locked) {
        xSemaphoreGive(sharedSpiMutex);
      }
    }

  private:
    bool locked = false;
  };

  inline void init() {
    SpiBusLock lock;
    tft.init();
    tft.setRotation(3); // 180-degree flipped layout direction
    tft.clear(TFT_BLACK);
  }

  inline void drawBrowser(const char* currentPath, String items[], bool isFolder[],
                          int itemCount, int selectedIndex, int firstVisible) {
    SpiBusLock lock;
    tft.clear(TFT_BLACK);
    tft.startWrite(); 

    tft.setTextColor(TFT_ORANGE);
    tft.setTextSize(1);
    tft.setCursor(5, 2);
    tft.printf("Dir: %s", currentPath);
    tft.drawLine(0, 14, 160, 14, TFT_DARKGRAY);

    constexpr int visibleItems = AppConfig::BrowserVisibleItems;
    int startY = 20;
    for (int row = 0; row < visibleItems; row++) {
      int i = firstVisible + row;
      if (i >= itemCount) break;
      int itemY = startY + (row * 14);
      if (itemY > 120) break; 

      if (i == selectedIndex) {
        tft.setTextColor(TFT_YELLOW);
        tft.setCursor(3, itemY);
        tft.print(">");
      }

      tft.setCursor(15, itemY);
      if (isFolder[i]) {
        tft.setTextColor(TFT_CYAN);
        tft.printf("[%s]", items[i].c_str());
      } else {
        tft.setTextColor(TFT_WHITE);
        String title = BrowserUtils::displayTitleFromFilename(items[i]);
        tft.print(title.c_str());
      }
    }
    tft.endWrite(); 
  }

  // drawJpg()/drawPng()'s maxWidth/maxHeight parameters only clip the
  // destination region - they do not scale the source image down to fit
  // it. Embedded cover art is almost always much larger than this 50x50
  // thumbnail box (300x300+ is typical), so without an explicit scale
  // factor only the image's top-left 50x50 corner would ever be visible.
  // These parse just enough of each format's header to compute one.

  // JPEG stores dimensions in its SOFn (start-of-frame) marker segment;
  // walk the marker chain until one is found (DHT/DAC/restart markers are
  // skipped since they aren't SOF markers despite falling in the 0xc0-0xcf
  // range).
  inline bool jpegDimensions(const uint8_t* data, size_t size, uint16_t& width,
                            uint16_t& height) {
    size_t offset = 2;  // skip the SOI marker (0xffd8)
    while (offset + 4 <= size) {
      if (data[offset] != 0xff) {
        ++offset;
        continue;
      }
      const uint8_t marker = data[offset + 1];
      if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
        offset += 2;
        continue;
      }
      if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
          marker != 0xc8 && marker != 0xcc) {
        if (offset + 9 > size) return false;
        height = (static_cast<uint16_t>(data[offset + 5]) << 8) | data[offset + 6];
        width = (static_cast<uint16_t>(data[offset + 7]) << 8) | data[offset + 8];
        return width > 0 && height > 0;
      }
      if (offset + 4 > size) return false;
      const uint16_t segmentLength =
          (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
      offset += 2 + segmentLength;
    }
    return false;
  }

  // PNG's IHDR chunk is always the first one, immediately after the 8-byte
  // signature: length(4) + "IHDR"(4) + width(4, big-endian) + height(4,
  // big-endian).
  inline bool pngDimensions(const uint8_t* data, size_t size, uint16_t& width,
                           uint16_t& height) {
    if (size < 24) return false;
    width = static_cast<uint16_t>((static_cast<uint32_t>(data[16]) << 24) |
                                  (static_cast<uint32_t>(data[17]) << 16) |
                                  (static_cast<uint32_t>(data[18]) << 8) |
                                  data[19]);
    height = static_cast<uint16_t>((static_cast<uint32_t>(data[20]) << 24) |
                                   (static_cast<uint32_t>(data[21]) << 16) |
                                   (static_cast<uint32_t>(data[22]) << 8) |
                                   data[23]);
    return width > 0 && height > 0;
  }

  // The straightforward "pass a scale_x/scale_y to drawJpg/drawPng" approach
  // (this function's previous version) hits a real LovyanGFX bug: passing
  // any scale != 1.0f makes it decode through its "affine" pixel path
  // (LGFXBase.inl's jpg_push_image_affine), whose color converter
  // (create_pc<T>()) only special-cases rgb565_t/rgb888_t/argb8888_t/
  // grayscale_t - NOT the bgr888_t type the JPEG/PNG decoders actually
  // produce - so it silently falls through to a generic byte copy that
  // swaps red and blue. The *unscaled* (scale==1.0f) path uses a different,
  // correctly-configured pixelcopy_t and renders colors fine. So: decode
  // into an off-screen sprite at the image's own native size (always
  // scale==1.0f, always correct colors), then let the sprite's own
  // pushRotateZoom() - a wholly separate, already-correctly-typed scaling
  // code path - do the scaled blit onto the real screen.
  inline void renderScaledImage(bool isPng, const uint8_t* imageBuffer,
                                size_t imageSize, int32_t destX, int32_t destY,
                                int32_t boxSize) {
    uint16_t width = 0, height = 0;
    const bool haveDims =
        isPng ? pngDimensions(imageBuffer, imageSize, width, height)
              : jpegDimensions(imageBuffer, imageSize, width, height);
    // Cap the sprite so a corrupt/unexpectedly huge image can't exhaust
    // PSRAM - falls back to an unscaled (but always correctly-colored) crop
    // in that case, same as when dimensions can't be parsed at all.
    constexpr uint16_t MaxSpriteSide = 1024;
    if (!haveDims || width > MaxSpriteSide || height > MaxSpriteSide) {
      if (isPng) {
        tft.drawPng(imageBuffer, imageSize, destX, destY, boxSize, boxSize);
      } else {
        tft.drawJpg(imageBuffer, imageSize, destX, destY, boxSize, boxSize);
      }
      return;
    }

    LGFX_Sprite art(&tft);
    art.setPsram(true);
    art.setColorDepth(16);
    if (art.createSprite(width, height) == nullptr) {
      // PSRAM allocation failed (fragmentation/low memory) - same
      // always-correct-colors fallback as above.
      if (isPng) {
        tft.drawPng(imageBuffer, imageSize, destX, destY, boxSize, boxSize);
      } else {
        tft.drawJpg(imageBuffer, imageSize, destX, destY, boxSize, boxSize);
      }
      return;
    }

    const bool decoded = isPng ? art.drawPng(imageBuffer, imageSize)
                               : art.drawJpg(imageBuffer, imageSize);
    if (decoded) {
      const uint16_t longestSide = width > height ? width : height;
      float scale = static_cast<float>(boxSize) / static_cast<float>(longestSide);
      if (scale > 1.0f) scale = 1.0f;
      art.setPivot(0, 0);
      art.pushRotateZoom(&tft, destX, destY, 0.0f, scale, scale);
    }
    art.deleteSprite();
  }

  inline void renderAlbumArt(const uint8_t* imageBuffer, size_t imageSize) {
    constexpr int32_t BoxSize = 50;
    if (imageSize >= 2 && imageBuffer[0] == 0xff && imageBuffer[1] == 0xd8) {
      renderScaledImage(false, imageBuffer, imageSize, 10, 40, BoxSize);
    } else if (imageSize >= 8 && imageBuffer[0] == 0x89 &&
               imageBuffer[1] == 'P' && imageBuffer[2] == 'N' &&
               imageBuffer[3] == 'G') {
      renderScaledImage(true, imageBuffer, imageSize, 10, 40, BoxSize);
    } else {
      tft.fillRect(10, 40, BoxSize, BoxSize, TFT_NAVY);
    }
  }

  inline void showPlayingScreen(const char* trackName, bool isPaused, uint8_t volume, const char* eqName,
                               bool hasCover = false, const uint8_t* coverData = nullptr, size_t coverSize = 0) {
    SpiBusLock lock;
    tft.clear(TFT_BLACK);
    tft.startWrite();
    
    tft.fillRoundRect(2, 2, 156, 124, 4, TFT_DARKGRAY);
    tft.drawRoundRect(2, 2, 156, 124, 4, TFT_LIGHTGRAY);
    
    tft.setTextColor(TFT_GREEN);
    tft.setCursor(8, 6);
    tft.print("NOW PLAYING:");
    
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(8, 18);
    
    String nameStr = String(trackName);
    if (nameStr.length() > 22) nameStr = nameStr.substring(0, 19) + "...";
    tft.print(nameStr);

    tft.drawRect(9, 39, 52, 52, TFT_WHITE);
    if (hasCover && coverData != nullptr && coverSize > 0) {
      tft.endWrite();
      renderAlbumArt(coverData, coverSize);
      tft.startWrite();
    } else {
      tft.fillRect(10, 40, 50, 50, TFT_NAVY);
      tft.setTextColor(TFT_CYAN);
      tft.setCursor(26, 58);
      tft.print("♫"); // Placeholder note icon if no artwork
    }

    tft.setTextColor(TFT_CYAN);
    tft.setCursor(66, 42);
    tft.printf("VOL: %d/21", volume);

    tft.setCursor(66, 56);
    tft.printf("EQ: %s", eqName);

    tft.setCursor(66, 74);
    if (isPaused) {
      tft.setTextColor(TFT_RED);
      tft.print("|| PAUSED");
    } else {
      tft.setTextColor(TFT_YELLOW);
      tft.print("> PLAYING");
    }

    tft.setTextColor(TFT_LIGHTGRAY);
    tft.setCursor(8, 98);
    tft.print("DN: EQ Menu | P5: Stop");
    tft.setCursor(8, 110);
    tft.print("P6:Vol- | P7:Vol+");
    tft.endWrite();
  }

  inline void drawEQMenu(int selectedIndex, int totalPresets, const char* getPresetNameFunc(int)) {
    SpiBusLock lock;
    tft.clear(TFT_BLACK);
    tft.startWrite();

    tft.setTextColor(TFT_MAGENTA);
    tft.setTextSize(1);
    tft.setCursor(5, 2);
    tft.print("EQUALIZER SETTINGS");
    tft.drawLine(0, 14, 160, 14, TFT_DARKGRAY);

    int startY = 24;
    for (int i = 0; i < totalPresets; i++) {
      int itemY = startY + (i * 18);

      if (i == selectedIndex) {
        tft.setTextColor(TFT_YELLOW);
        tft.setCursor(5, itemY);
        tft.print(">");
        tft.setTextColor(TFT_WHITE);
      } else {
        tft.setTextColor(TFT_DARKGRAY);
      }

      tft.setCursor(20, itemY);
      tft.print(getPresetNameFunc(i));
    }

    tft.setTextColor(TFT_LIGHTGRAY);
    tft.setCursor(5, 110);
    tft.print("UP/DN: Move | SEL: Apply");
    tft.setCursor(5, 120);
    tft.print("BACK: Return to Player");

    tft.endWrite();
  }
}