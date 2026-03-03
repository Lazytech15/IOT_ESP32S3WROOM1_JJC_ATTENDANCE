// ══════════════════════════════════════════════════════════════════════════════
// TFTDisplayManager.cpp
// ──────────────────────────────────────────────────────────────────────────────
// Color philosophy (matches reference clock-dashboard project):
//
//   platformio.ini:  TFT_RGB_ORDER=TFT_RGB   ← no BGR swap, no compensation
//   init():          invertDisplay(false)      ← no inversion
//
//   All colors are plain tft.color565(R, G, B) — what you write is what
//   appears on screen. Orange (255,140,0) → orange on screen. Full stop.
//
//   Theme / accent switching follows the same initColors() + applyTheme() +
//   applyAccent() pattern used in the reference project's globals/tft_ui.
// ══════════════════════════════════════════════════════════════════════════════

#include "TFTDisplayManager.h"
#include "pin_assignments.h"

// ── Static member definitions ─────────────────────────────────────────────────
TFT_eSPI* TFTDisplayManager::_tft              = nullptr;
bool      TFTDisplayManager::_initialized      = false;
uint8_t   TFTDisplayManager::_currentBrightness = 255;
uint8_t   TFTDisplayManager::_rotation         = TFT_ROTATION;

// ── TFTColors — zero-initialised; initColors() fills real values ──────────────
namespace TFTColors {
    uint16_t BG_DARK        = 0x0000;
    uint16_t BG_MID         = 0x0000;
    uint16_t BG_LIGHT       = 0x0000;

    uint16_t ACCENT_TEAL    = 0x0000;
    uint16_t ACCENT_CYAN    = 0x0000;
    uint16_t ACCENT_ORANGE  = 0x0000;
    uint16_t ACCENT_PURPLE  = 0x0000;
    uint16_t ACCENT_PINK    = 0x0000;
    uint16_t ACCENT_YELLOW  = 0x0000;

    uint16_t TEXT_PRIMARY   = 0xFFFF;
    uint16_t TEXT_SECONDARY = 0xFFFF;
    uint16_t TEXT_DIM       = 0x0000;

    uint16_t SUCCESS        = 0x0000;
    uint16_t ERROR          = 0x0000;
    uint16_t WARNING        = 0x0000;
    uint16_t INFO           = 0x0000;

    uint16_t BORDER_BRIGHT  = 0xFFFF;
    uint16_t BORDER_DIM     = 0x0000;

    uint16_t WHITE          = 0xFFFF;
    uint16_t BLACK          = 0x0000;
    uint16_t RED            = 0xF800;
    uint16_t GREEN          = 0x07E0;
    uint16_t BLUE           = 0x001F;
    uint16_t YELLOW         = 0xFFE0;
    uint16_t CYAN           = 0x07FF;
    uint16_t MAGENTA        = 0xF81F;
}

// ── initColors ────────────────────────────────────────────────────────────────
// Plain color565(R, G, B) — no BGR swap, no invert compensation needed.
// TFT_RGB_ORDER=TFT_RGB + invertDisplay(false) = colors render exactly as here.
// ─────────────────────────────────────────────────────────────────────────────
void TFTDisplayManager::initColors() {
    using namespace TFTColors;

    // Backgrounds — pure black base with dark orange-tinted cards
    BG_DARK  = _tft->color565(  0,   0,   0);   // pure black
    BG_MID   = _tft->color565( 20,  12,   0);   // very dark orange-black (card bg)
    BG_LIGHT = _tft->color565( 40,  22,   0);   // dark amber (stat card bg)

    // Orange accent palette
    ACCENT_TEAL   = _tft->color565(255, 140,   0);   // primary orange
    ACCENT_CYAN   = _tft->color565(255, 180,  50);   // light orange
    ACCENT_ORANGE = _tft->color565(255, 100,   0);   // deep orange
    ACCENT_PURPLE = _tft->color565(255, 160,  30);   // warm amber
    ACCENT_PINK   = _tft->color565(255,  80,   0);   // red-orange
    ACCENT_YELLOW = _tft->color565(255, 220,   0);   // yellow

    // Text
    TEXT_PRIMARY   = _tft->color565(255, 255, 255);   // white
    TEXT_SECONDARY = _tft->color565(255, 200, 120);   // warm orange-white
    TEXT_DIM       = _tft->color565(140,  80,  20);   // dim amber

    // Status
    SUCCESS = _tft->color565(  0, 210, 100);   // green
    ERROR   = _tft->color565(220,  50,  50);   // red
    WARNING = _tft->color565(255, 180,   0);   // amber
    INFO    = _tft->color565(255, 140,   0);   // orange

    // Borders
    BORDER_BRIGHT = _tft->color565(255, 140,   0);   // orange
    BORDER_DIM    = _tft->color565( 60,  30,   0);   // dark orange-brown

    // Standard
    WHITE   = 0xFFFF;
    BLACK   = 0x0000;
    RED     = _tft->color565(220,  40,  40);
    GREEN   = _tft->color565(  0, 200,  80);
    BLUE    = _tft->color565( 30, 100, 220);
    YELLOW  = _tft->color565(255, 220,   0);
    CYAN    = _tft->color565(  0, 220, 230);
    MAGENTA = _tft->color565(200,  40, 180);
}

// ── init ──────────────────────────────────────────────────────────────────────
// Mirrors the reference project setup() exactly:
//   tft.init() → invertDisplay(false) → setRotation() → fillScreen(BLACK) → initColors()
// ─────────────────────────────────────────────────────────────────────────────
bool TFTDisplayManager::init(uint8_t rotation, bool /*invertColors — unused*/) {
    if (_initialized) return true;

    Serial.println("[TFT] init() start");

    // ── GPIO 9 = TFT_BL (backlight) ──────────────────────────────────────────
    // MUST be configured as OUTPUT before any digitalWrite() call, otherwise
    // ESP32 logs: "IO 9 is not set as GPIO" and may crash / not light the panel.
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);   // backlight ON immediately
    Serial.println("[TFT] Backlight GPIO9 → OUTPUT HIGH");

    _tft = new TFT_eSPI();
    if (!_tft) {
        Serial.println("[TFT] ERROR: new TFT_eSPI() returned null!");
        return false;
    }

    _tft->init();
    Serial.println("[TFT] tft.init() done");

    _tft->invertDisplay(false);
    _tft->setRotation(rotation);
    _rotation = rotation;
    Serial.printf("[TFT] rotation=%d  size=%dx%d\n",
                  rotation, _tft->width(), _tft->height());

    _tft->fillScreen(TFT_BLACK);

    // Init colors AFTER tft is ready so color565() works
    initColors();
    Serial.println("[TFT] colors initialised");

    _currentBrightness = 255;
    _initialized = true;
    Serial.println("[TFTDisplayManager] ✅ init OK — TFT_RGB, invertDisplay(false)");
    return true;
}

// ── Accessors ─────────────────────────────────────────────────────────────────
TFT_eSPI* TFTDisplayManager::getTFT()        { return _tft; }
bool      TFTDisplayManager::isInitialized() { return _initialized; }
uint8_t   TFTDisplayManager::getRotation()   { return _rotation; }
int16_t   TFTDisplayManager::getWidth()  { return _tft ? (int16_t)_tft->width()  : SCREEN_WIDTH;  }
int16_t   TFTDisplayManager::getHeight() { return _tft ? (int16_t)_tft->height() : SCREEN_HEIGHT; }

// ── Backlight (GPIO 9 hardwired to 3V3 — no software control) ────────────────
void TFTDisplayManager::setBacklight(uint8_t brightness) {
    _currentBrightness = brightness;
    // GPIO9 must have been set OUTPUT in init() — safe to call digitalWrite here.
    digitalWrite(TFT_BL, brightness > 0 ? HIGH : LOW);
}
void TFTDisplayManager::backlightOn()  { setBacklight(255); }
void TFTDisplayManager::backlightOff() { setBacklight(0); }
void TFTDisplayManager::fadeBacklight(uint8_t target, uint16_t durationMs) {
    int step  = (target > _currentBrightness) ? 1 : -1;
    int steps = abs((int)target - (int)_currentBrightness);
    if (steps == 0) return;
    uint32_t delayPerStep = durationMs / steps;
    for (int b = _currentBrightness; b != target; b += step) {
        setBacklight((uint8_t)b);
        delay(delayPerStep);
    }
    setBacklight(target);
}

// ── Color utility ─────────────────────────────────────────────────────────────
uint16_t TFTDisplayManager::color565(uint8_t r, uint8_t g, uint8_t b) {
    return _tft ? _tft->color565(r, g, b)
                : ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// ── Drawing utilities ─────────────────────────────────────────────────────────
void TFTDisplayManager::clearScreen(uint16_t color) {
    if (_tft) _tft->fillScreen(color);
}
void TFTDisplayManager::drawSeparator(int16_t y, uint16_t color) {
    if (_tft) _tft->drawFastHLine(0, y, _tft->width(), color);
}
void TFTDisplayManager::drawVerticalSeparator(int16_t x, uint16_t color) {
    if (_tft) _tft->drawFastVLine(x, 0, _tft->height(), color);
}
void TFTDisplayManager::drawCard(int16_t x, int16_t y, int16_t w, int16_t h,
                                  uint16_t bgColor, uint16_t borderColor,
                                  uint8_t cornerRadius) {
    if (!_tft) return;
    _tft->fillRoundRect(x, y, w, h, cornerRadius, bgColor);
    _tft->drawRoundRect(x, y, w, h, cornerRadius, borderColor);
}
void TFTDisplayManager::drawModernCard(int16_t x, int16_t y, int16_t w, int16_t h,
                                        uint16_t bgColor, uint16_t borderColor,
                                        uint16_t accentColor, uint8_t cornerRadius) {
    if (!_tft) return;
    _tft->fillRoundRect(x, y, w, h, cornerRadius, bgColor);
    _tft->drawRoundRect(x, y, w, h, cornerRadius, borderColor);
    _tft->fillRect(x, y + cornerRadius, 3, h - 2 * cornerRadius, accentColor);
}
void TFTDisplayManager::drawCenteredText(const char* text, int16_t x, int16_t y,
                                          uint8_t font, uint16_t textColor, uint16_t bgColor) {
    if (!_tft) return;
    _tft->setTextDatum(MC_DATUM);
    _tft->setTextColor(textColor, bgColor);
    _tft->drawString(text, x, y, font);
    _tft->setTextDatum(TL_DATUM);
}
void TFTDisplayManager::drawWrappedText(const char* text, int16_t x, int16_t y,
                                         int16_t maxWidth, uint8_t font,
                                         uint16_t textColor, uint16_t bgColor) {
    if (!_tft) return;
    _tft->setTextColor(textColor, bgColor);
    String src(text), line = "";
    int16_t lineH = _tft->fontHeight(font), curY = y, start = 0;
    while (start <= (int)src.length()) {
        int space = src.indexOf(' ', start);
        if (space < 0) space = src.length();
        String word = src.substring(start, space);
        String test = line.length() > 0 ? (line + " " + word) : word;
        if (_tft->textWidth(test, font) > maxWidth && line.length() > 0) {
            _tft->drawString(line, x, curY, font);
            curY += lineH; line = word;
        } else { line = test; }
        start = space + 1;
    }
    if (line.length() > 0) _tft->drawString(line, x, curY, font);
}
void TFTDisplayManager::drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h,
                                         uint8_t percentage, uint16_t fillColor,
                                         uint16_t bgColor, uint16_t borderColor) {
    if (!_tft) return;
    _tft->fillRect(x, y, w, h, bgColor);
    _tft->drawRect(x, y, w, h, borderColor);
    int16_t filled = (int16_t)(((int32_t)w - 2) * percentage / 100);
    if (filled > 0) _tft->fillRect(x + 1, y + 1, filled, h - 2, fillColor);
}
void TFTDisplayManager::drawLoadingSpinner(int16_t x, int16_t y, uint8_t radius,
                                            uint16_t color, uint8_t thickness) {
    if (!_tft) return;
    static uint8_t spinAngle = 0;
    spinAngle = (spinAngle + 30) % 360;
    float startRad = spinAngle * DEG_TO_RAD;
    float endRad   = (spinAngle + 270) * DEG_TO_RAD;
    for (float a = startRad; a <= endRad; a += 0.1f) {
        _tft->fillCircle(x + (int16_t)(radius * cosf(a)),
                         y + (int16_t)(radius * sinf(a)),
                         thickness / 2, color);
    }
}

// ── Text utilities ────────────────────────────────────────────────────────────
int16_t TFTDisplayManager::getTextWidth(const char* text, uint8_t font) {
    return _tft ? (int16_t)_tft->textWidth(text, font) : 0;
}
int16_t TFTDisplayManager::getTextHeight(uint8_t font) {
    return _tft ? (int16_t)_tft->fontHeight(font) : 0;
}
String TFTDisplayManager::truncateText(const String& text, int16_t maxWidth,
                                        uint8_t font, const char* ellipsis) {
    if (!_tft || _tft->textWidth(text.c_str(), font) <= maxWidth) return text;
    String result = "";
    for (unsigned int i = 0; i < text.length(); i++) {
        String test = text.substring(0, i + 1);
        if (_tft->textWidth(test.c_str(), font) +
            _tft->textWidth(ellipsis, font) > maxWidth)
            return result + String(ellipsis);
        result = test;
    }
    return result;
}

// ── Info ──────────────────────────────────────────────────────────────────────
void TFTDisplayManager::printInfo() {
    Serial.println("[TFTDisplayManager] ────────────────────────");
    Serial.printf ("  Initialized : %s\n", _initialized ? "yes" : "no");
    if (_tft) {
        Serial.printf("  Width       : %d px\n", _tft->width());
        Serial.printf("  Height      : %d px\n", _tft->height());
        Serial.printf("  Rotation    : %d\n",    _rotation);
    }
    Serial.println("[TFTDisplayManager] ────────────────────────");
}