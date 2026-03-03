// ══════════════════════════════════════════════════════════════════════════════
// pin_assignments.h
// Pin Configuration — ESP32-S3 WROOM-1 + ST7789 2.8" TFT + PN532 NFC
//
// CHANGES vs original:
//   • Removed camera pins (ESP32-S3 WROOM-1 has no camera connector)
//   • Verified all pins are safe on ESP32-S3 WROOM-1 (avoid strapping pins)
//   • SD card pins updated to safe WROOM-1 GPIOs
//
// STRAPPING PINS TO AVOID (boot sensitive):
//   GPIO 0, 3, 45, 46 — do NOT use for peripherals
// ══════════════════════════════════════════════════════════════════════════════

#ifndef PIN_ASSIGNMENTS_H
#define PIN_ASSIGNMENTS_H

// ══════════════════════════════════════════════════════════════════════════════
// TFT DISPLAY (ST7789 2.8" — TPM408) — Custom SPI (FSPI)
// ══════════════════════════════════════════════════════════════════════════════
#define TFT_MOSI        4    // SPI Data Out
#define TFT_SCK         5    // SPI Clock
#define TFT_CS          6    // Chip Select
#define TFT_DC          7    // Data / Command
#define TFT_RST         8    // Reset
#define TFT_BL          9    // Backlight (HIGH = on)

// ══════════════════════════════════════════════════════════════════════════════
// NFC MODULE (PN532) — Software SPI
// ══════════════════════════════════════════════════════════════════════════════
#define PN532_SS        10   // Chip Select
#define PN532_MOSI      11   // SPI Data Out
#define PN532_SCK       12   // SPI Clock
#define PN532_MISO      13   // SPI Data In

// ══════════════════════════════════════════════════════════════════════════════
// SD CARD — SPI (Software or HSPI)
// Using GPIO 38-41 which are safe on WROOM-1 and away from camera/USB
// ══════════════════════════════════════════════════════════════════════════════
#define SD_MOSI         35   // SD Data In
#define SD_SCK          36   // SD Clock
#define SD_MISO         37   // SD Data Out
#define SD_CS           38   // SD Chip Select

// ══════════════════════════════════════════════════════════════════════════════
// PIN SAFETY NOTES FOR ESP32-S3 WROOM-1
// ══════════════════════════════════════════════════════════════════════════════
/*
 * SAFE GPIOs confirmed for ESP32-S3 WROOM-1 (N8 / N8R8):
 *   Input + Output: 1-21, 35-48 (with exceptions below)
 *
 * AVOID or use with caution:
 *   GPIO 0   — Strapping pin (boot mode)
 *   GPIO 3   — Strapping pin
 *   GPIO 45  — Strapping pin (VDD_SPI voltage)
 *   GPIO 46  — Strapping pin (ROM messages)
 *   GPIO 19  — USB D- (if using USB-OTG)
 *   GPIO 20  — USB D+ (if using USB-OTG)
 *   GPIO 26-32 — Connected to SPI flash (do NOT use)
 *   GPIO 33-37 — Connected to PSRAM on R8 variant (do NOT use if you have PSRAM)
 *
 * NOTE: If your board is N8R8 (with PSRAM), GPIOs 33-37 are used by PSRAM.
 * In that case, change SD_MOSI/SCK/MISO to GPIO 39/40/41 and SD_CS to 42.
 *
 * ┌──────┬─────────────────────────────────────────────────────┐
 * │ GPIO │ Function                                            │
 * ├──────┼─────────────────────────────────────────────────────┤
 * │   4  │ TFT_MOSI                                            │
 * │   5  │ TFT_SCK                                             │
 * │   6  │ TFT_CS                                              │
 * │   7  │ TFT_DC                                              │
 * │   8  │ TFT_RST                                             │
 * │   9  │ TFT_BL (Backlight)                                  │
 * ├──────┼─────────────────────────────────────────────────────┤
 * │  10  │ PN532_SS                                            │
 * │  11  │ PN532_MOSI                                          │
 * │  12  │ PN532_SCK                                           │
 * │  13  │ PN532_MISO                                          │
 * ├──────┼─────────────────────────────────────────────────────┤
 * │  35  │ SD_MOSI  (use 39 if N8R8 PSRAM variant)            │
 * │  36  │ SD_SCK   (use 40 if N8R8 PSRAM variant)            │
 * │  37  │ SD_MISO  (use 41 if N8R8 PSRAM variant)            │
 * │  38  │ SD_CS    (use 42 if N8R8 PSRAM variant)            │
 * └──────┴─────────────────────────────────────────────────────┘
 */

#endif // PIN_ASSIGNMENTS_H