// User_Setup.h for TFT_eSPI Library
// ESP32-WROOM-32 BT Audio Effects Granular Processor

// ================================================================
// Display Driver Selection
// ================================================================
// Select the display driver IC
#define ILI9341_DRIVER      // Generic ILI9341 240x320 TFT display

// ================================================================
// Display Resolution
// ================================================================
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// ================================================================
// ESP32 GPIO Pin Definitions for TFT Display
// ================================================================
#define TFT_MOSI 23  // SPI MOSI - Data Out
#define TFT_SCLK 18  // SPI Clock
#define TFT_CS   5   // Chip Select (can be set to -1 if not used)
#define TFT_DC   4   // Data/Command select
#define TFT_RST  22  // Reset pin (could connect to RST pin of ESP32)

// ================================================================
// Display Backlight Control (Optional)
// ================================================================
// If your display has a backlight control pin, uncomment and set:
// #define TFT_BL   32  // LED back-light control pin
// #define TFT_BACKLIGHT_ON HIGH  // Level to turn ON back-light (HIGH or LOW)

// ================================================================
// SPI Frequency
// ================================================================
// Set SPI frequency for reading and writing
#define SPI_FREQUENCY       40000000  // 40 MHz for normal operation
#define SPI_READ_FREQUENCY  20000000  // 20 MHz for reading (slower for stability)
#define SPI_TOUCH_FREQUENCY  2500000  // 2.5 MHz for touch controller if used

// ================================================================
// Font Settings
// ================================================================
// Load default fonts
#define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
#define LOAD_FONT2  // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
#define LOAD_FONT4  // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters
#define LOAD_FONT6  // Font 6. Large 48 pixel font, needs ~2666 bytes in FLASH, only characters 1234567890:-.apm
#define LOAD_FONT7  // Font 7. 7 segment 48 pixel font, needs ~2438 bytes in FLASH, only characters 1234567890:.
#define LOAD_FONT8  // Font 8. Large 75 pixel font needs ~3256 bytes in FLASH, only characters 1234567890:-.
#define LOAD_GFXFF  // FreeFonts. Include access to the 48 Adafruit_GFX free fonts

// ================================================================
// Display Color Settings
// ================================================================
// Uncomment to enable color inversion (if colors appear reversed)
// #define TFT_INVERSION_ON
// #define TFT_INVERSION_OFF

// ================================================================
// SPI Port Selection (ESP32)
// ================================================================
// Use VSPI (default) or HSPI
// #define USE_HSPI_PORT

// ================================================================
// Additional ESP32 Specific Settings
// ================================================================
// Optionally reduce SPI DMA buffer size if memory is tight
// #define SPI_DMA_BYTES_PER_PIXEL 2

// ================================================================
// Touch Screen Settings (if using touch)
// ================================================================
// Uncomment if you have a touch screen and configure pins
// #define TOUCH_CS 21  // Chip select pin for touch controller

// ================================================================
// End of User_Setup.h
// ================================================================
