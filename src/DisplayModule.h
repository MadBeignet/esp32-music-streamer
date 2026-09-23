#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "PinConfig.h"
#include "AppConfig.h"
#include "BrowserUtils.h"

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
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

extern LGFX_XiaoS3_ST7735 tft;

namespace Display {
  inline void init() {
    tft.init();
    tft.setRotation(3); // 180-degree flipped layout direction
    tft.clear(TFT_BLACK);
  }

  inline void drawBrowser(const char* currentPath, String items[], bool isFolder[],
                          int itemCount, int selectedIndex, int firstVisible) {
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

  inline void renderAlbumArt(const uint8_t* imageBuffer, size_t imageSize) {
    tft.startWrite();
    tft.drawJpg(imageBuffer, imageSize, 10, 40, 50, 50);
    tft.endWrite();
  }

  inline void showPlayingScreen(const char* trackName, bool isPaused, uint8_t volume, const char* eqName, bool hasCover = false) {
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
    if (!hasCover) {
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