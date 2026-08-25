// ══════════════════════════════════════════════════════════════════════════════
// screensaver.h  — Portrait 240×320
// ══════════════════════════════════════════════════════════════════════════════
// Purpose: Replace the old "backlight off" timeout behavior with a low-burn
// screensaver: company logo + big HH:MM:SS clock + date, all on a black
// background. The panel is an SPI ST7789 with TFT_BL hardwired to a GPIO
// that's always driven HIGH by TFTDisplayManager — there is no way to fully
// dim it in hardware without cutting a trace, so instead of a dim gray
// "off-but-not-really-off" screen we keep the backlight on and just show
// mostly-black content, which is the practical equivalent for this panel.
//
// Logo source: /logo.jpg on the SD card (SD_MMC), decoded with TJpg_Decoder
// — the same library/pattern already used by EmployeeProfileDisplay for
// profile photos. Keeping the logo on the SD card (instead of compiling it
// into flash) means it can be swapped any time by just replacing the file.
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

#ifndef SCREEN_W
#define SCREEN_W 240
#endif
#ifndef SCREEN_H
#define SCREEN_H 320
#endif

// Path on the SD card (root) where the logo JPEG must be placed.
#define SCREENSAVER_LOGO_PATH   "/logo.jpg"

// ── Layout (portrait 240×320) ───────────────────────────────────────────────
#define SS_LOGO_Y        20     // top y where the logo is drawn
#define SS_LOGO_MAX_W    150    // safety cap so an oversized jpg can't overflow
#define SS_LOGO_MAX_H    150
#define SS_TIME_Y        205    // vertical center of the HH:MM:SS line
#define SS_DATE_Y         250    // vertical center of the date line

// ══════════════════════════════════════════════════════════════════════════════
// API
// ══════════════════════════════════════════════════════════════════════════════

// Call once from setup(), after both TFTDisplayManager::init() and the SD
// card have been brought up. Registers the TJpg_Decoder callback and checks
// whether /logo.jpg exists so drawScreensaver() doesn't have to re-check
// every time it's called.
void screensaverInit();

// Full redraw: black background + logo + time + date.
// Call once when entering screensaver mode (i.e. right when the timeout
// fires). For per-second updates while already in screensaver mode, use
// updateScreensaverClock()/updateScreensaverDate() instead — they only
// repaint their own text region, so there's no flicker.
void drawScreensaver();

// Repaints just the HH:MM:SS line (military/24h format).
void updateScreensaverClock(uint8_t h, uint8_t m, uint8_t s);
void updateScreensaverClockUnsynced();

// Repaints just the date line, e.g. "August 19, 2026".
void updateScreensaverDate(const String& dateStr);

// True if /logo.jpg was found on the SD card at screensaverInit() time.
bool screensaverLogoAvailable();
