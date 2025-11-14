// User_Setup.h
// TFT_eSPI library configuration for ESP32-WROOM-32 with TFT Display

// ##################################################################################
//
// Section 1: Call up the right driver file and any options for it
//
// ##################################################################################

// Define the driver for your display
// Uncomment the line that matches your display type
// #define ILI9341_DRIVER       // Generic driver for common displays
// #define ST7735_DRIVER        // For ST7735 1.8" TFT
// #define ILI9163_DRIVER       // For ILI9163 1.44" TFT
// #define S6D02A1_DRIVER       // For S6D02A1 1.54" TFT
// #define ILI9486_DRIVER       // For ILI9486 3.5" TFT
// #define ST7789_DRIVER        // For ST7789 TFT (240x240 or 240x320)
// #define ST7789_2_DRIVER      // For ST7789 240x320 or 240x240 displays

#define ILI9341_DRIVER       // Default to ILI9341 (2.4" or 2.8" TFT)

// ##################################################################################
//
// Section 2: Display orientation and color order
//
// ##################################################################################

// Uncomment to use inverted display
// #define TFT_INVERSION_ON
// #define TFT_INVERSION_OFF

// Color order (RGB or BGR)
#define TFT_RGB_ORDER TFT_BGR  // Colour order Blue-Green-Red

// ##################################################################################
//
// Section 3: Font settings
//
// ##################################################################################

#define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
#define LOAD_FONT2  // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
#define LOAD_FONT4  // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters
#define LOAD_FONT6  // Font 6. Large 48 pixel font, needs ~2666 bytes in FLASH, only characters 1234567890:-.apm
#define LOAD_FONT7  // Font 7. 7 segment 48 pixel font, needs ~2438 bytes in FLASH, only characters 1234567890:-.
#define LOAD_FONT8  // Font 8. Large 75 pixel font needs ~3256 bytes in FLASH, only characters 1234567890:-.
#define LOAD_GFXFF  // FreeFonts. Include access to the 48 Adafruit_GFX free fonts FF1 to FF48 and custom fonts

#define SMOOTH_FONT

// ##################################################################################
//
// Section 4: ESP32 GPIO pin configuration for SPI interface
//
// ##################################################################################

// ESP32-WROOM-32 to TFT Display GPIO Pin Connections
// ===================================================
// MOSI (Data Output) : GPIO 23 → TFT_MOSI
// SCLK (Clock)       : GPIO 18 → TFT_SCLK
// CS (Chip Select)   : GPIO 5  → TFT_CS
// DC (Data/Command)  : GPIO 4  → TFT_DC
// RST (Reset)        : GPIO 22 → TFT_RST

#define TFT_MISO -1    // Not used for TFT display (can be set to -1 if not needed)
#define TFT_MOSI 23    // Data output (Master Out Slave In)
#define TFT_SCLK 18    // Clock signal
#define TFT_CS   5     // Chip select control pin
#define TFT_DC   4     // Data/Command control pin
#define TFT_RST  22    // Reset pin (could use -1 if not connected, then use software reset)

// ##################################################################################
//
// Section 5: SPI frequency settings
//
// ##################################################################################

// Frequency for writing to TFT
#define SPI_FREQUENCY       27000000  // 27 MHz (maximum is about 27MHz for ILI9341)

// Optional reduced SPI frequency for reading
#define SPI_READ_FREQUENCY  20000000  // 20 MHz

// The XPT2046 requires a lower SPI clock rate of 2.5MHz
// If you're using a touch controller, uncomment the line below
// #define SPI_TOUCH_FREQUENCY 2500000

// ##################################################################################
//
// Section 6: Optional touch screen configuration
//
// ##################################################################################

// Uncomment if you have a touch screen and want to use it
// #define TOUCH_CS 21  // Chip select pin for XPT2046 touch controller

// Touch calibration values (you'll need to calibrate for your specific display)
// #define TOUCH_CALIBRATION_X 300
// #define TOUCH_CALIBRATION_Y 3600
// #define TOUCH_CALIBRATION_ROTATE 2
// #define TOUCH_CALIBRATION_INVERT_X 0
// #define TOUCH_CALIBRATION_INVERT_Y 1

// ##################################################################################
//
// Section 7: Other options
//
// ##################################################################################

// Use HSPI port on ESP32 (default is VSPI)
// Uncomment to use HSPI instead of VSPI
// #define USE_HSPI_PORT

// ##################################################################################
// End of User_Setup.h
// ##################################################################################
