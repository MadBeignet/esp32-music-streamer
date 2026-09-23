#pragma once
#include <Arduino.h>

// SPI Shared Screen & SD Card Pin Indices (PlatformIO Native S3 Mapping)
#define TFT_MOSI 9   // Physical D10 (GPIO 9)
#define TFT_MISO 8   // Physical D9  (GPIO 8)
#define TFT_SCK  7   // Physical D8  (GPIO 7)
#define TFT_TDC  3   // Physical D2  (GPIO 3)
#define TFT_TCS  4   // Physical D3  (GPIO 4)
#define TFT_CCS  5   // Physical D4  (GPIO 5)
#define TFT_RST -1   // Kept physically tied to 3.3V

// I2S Hardware Audio Bus
#define I2S_BCLK 44  // Physical D7 (GPIO 44) -> Connects to PCM5102 'bck'
#define I2S_LRC  43  // Physical D6 (GPIO 43) -> Connects to PCM5102 'lck'
#define I2S_DOUT 6   // Physical D5 (GPIO 6)  -> Connects to PCM5102 'din'

// NEW ISOLATED SOFTWARE I2C PINS FOR THE MPR121
#define I2C_SDA  1   // Physical D0 (GPIO 1)
#define I2C_SCL  2   // Physical D1 (GPIO 2)
