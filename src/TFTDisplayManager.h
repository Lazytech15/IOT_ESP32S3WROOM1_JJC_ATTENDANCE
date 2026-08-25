// ══════════════════════════════════════════════════════════════════════════════
// TFTDisplayManager.h
// Reusable TFT Display Manager for ESP32-S3
// ══════════════════════════════════════════════════════════════════════════════
// Purpose: Centralize TFT_eSPI instance and provide reusable display utilities
// 
// Design Philosophy:
// - Single TFT_eSPI instance shared across all modules
// - Basic color palette for consistent UI
// - Utility functions for common drawing operations
// - No business logic - just display management
// 
// Usage:
//   TFTDisplayManager::init();
//   TFT_eSPI* tft = TFTDisplayManager::getTFT();
//   tft->drawString("Hello", 10, 10);
// ══════════════════════════════════════════════════════════════════════════════

#ifndef TFT_DISPLAY_MANAGER_H
#define TFT_DISPLAY_MANAGER_H

#include <Arduino.h>
#include <TFT_eSPI.h>

// ══════════════════════════════════════════════════════════════════════════════
// Display Configuration
// ══════════════════════════════════════════════════════════════════════════════

#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   320
#define TFT_ROTATION    0       // Portrait mode (0=portrait, 1=landscape)

// Backlight PWM (LEDC) — lets setBacklight()/fadeBacklight() dim smoothly
// instead of just switching the panel on/off.
#define BACKLIGHT_PWM_FREQ      5000   // Hz, well above flicker-visible range
#define BACKLIGHT_PWM_RES_BITS  8      // 0-255 duty range
#define BACKLIGHT_PWM_CHANNEL   0      // only used on arduino-esp32 < 3.0

// ══════════════════════════════════════════════════════════════════════════════
// Color Palette
// ══════════════════════════════════════════════════════════════════════════════

namespace TFTColors {
    // Background colors
    extern uint16_t BG_DARK;
    extern uint16_t BG_MID;
    extern uint16_t BG_LIGHT;
    
    // Accent colors
    extern uint16_t ACCENT_TEAL;
    extern uint16_t ACCENT_CYAN;
    extern uint16_t ACCENT_ORANGE;
    extern uint16_t ACCENT_PURPLE;
    extern uint16_t ACCENT_PINK;
    extern uint16_t ACCENT_YELLOW;
    
    // Text colors
    extern uint16_t TEXT_PRIMARY;
    extern uint16_t TEXT_SECONDARY;
    extern uint16_t TEXT_DIM;
    
    // Status colors
    extern uint16_t SUCCESS;
    extern uint16_t ERROR;
    extern uint16_t WARNING;
    extern uint16_t INFO;
    
    // Border colors
    extern uint16_t BORDER_BRIGHT;
    extern uint16_t BORDER_DIM;
    
    // Standard colors
    extern uint16_t WHITE;
    extern uint16_t BLACK;
    extern uint16_t RED;
    extern uint16_t GREEN;
    extern uint16_t BLUE;
    extern uint16_t YELLOW;
    extern uint16_t CYAN;
    extern uint16_t MAGENTA;
}

// ══════════════════════════════════════════════════════════════════════════════
// TFTDisplayManager - Static Class
// ══════════════════════════════════════════════════════════════════════════════

class TFTDisplayManager {
public:
    // ─────────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * Initialize TFT display
     * @param rotation Display rotation (0-3)
     * @param invertColors Whether to invert display colors
     * @return true if initialization successful
     */
    static bool init(uint8_t rotation = TFT_ROTATION, bool invertColors = false);
    
    /**
     * Get pointer to TFT instance
     * @return Pointer to TFT_eSPI instance (never null after init)
     */
    static TFT_eSPI* getTFT();
    
    /**
     * Check if display is initialized
     */
    static bool isInitialized();
    
    // ─────────────────────────────────────────────────────────────────────────
    // Backlight Control
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * Set backlight brightness (0-255)
     */
    static void setBacklight(uint8_t brightness);
    
    /**
     * Turn backlight on
     */
    static void backlightOn();
    
    /**
     * Turn backlight off
     */
    static void backlightOff();
    
    /**
     * Fade backlight to target brightness. Returns true if the fade
     * completed uninterrupted, false if it was aborted early (e.g. a tap
     * woke the screen mid-fade via backlightOn() on the other core) — the
     * caller should treat false as "didn't actually reach target" rather
     * than assuming the dim happened.
     * @param targetBrightness Target brightness (0-255)
     * @param durationMs Duration of fade in milliseconds
     */
    static bool fadeBacklight(uint8_t targetBrightness, uint16_t durationMs = 500);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Color Utilities
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * Initialize color palette
     * Called automatically by init(), but can be called again to refresh
     */
    static void initColors();
    
    /**
     * Create RGB565 color
     */
    static uint16_t color565(uint8_t r, uint8_t g, uint8_t b);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Drawing Utilities
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * Clear entire screen with color
     */
    static void clearScreen(uint16_t color = 0x0000);
    
    /**
     * Draw horizontal separator line
     */
    static void drawSeparator(int16_t y, uint16_t color);
    
    /**
     * Draw vertical separator line
     */
    static void drawVerticalSeparator(int16_t x, uint16_t color);
    
    /**
     * Draw rounded rectangle card
     */
    static void drawCard(int16_t x, int16_t y, int16_t w, int16_t h,
                        uint16_t bgColor, uint16_t borderColor,
                        uint8_t cornerRadius = 8);
    
    /**
     * Draw modern card with accent stripe
     */
    static void drawModernCard(int16_t x, int16_t y, int16_t w, int16_t h,
                              uint16_t bgColor, uint16_t borderColor,
                              uint16_t accentColor, uint8_t cornerRadius = 8);
    
    /**
     * Draw centered text
     */
    static void drawCenteredText(const char* text, int16_t x, int16_t y,
                                 uint8_t font, uint16_t textColor, uint16_t bgColor);
    
    /**
     * Draw text with automatic word wrap
     */
    static void drawWrappedText(const char* text, int16_t x, int16_t y,
                               int16_t maxWidth, uint8_t font,
                               uint16_t textColor, uint16_t bgColor);
    
    /**
     * Draw progress bar
     */
    static void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h,
                                uint8_t percentage, uint16_t fillColor,
                                uint16_t bgColor, uint16_t borderColor);
    
    /**
     * Draw loading spinner
     */
    static void drawLoadingSpinner(int16_t x, int16_t y, uint8_t radius,
                                   uint16_t color, uint8_t thickness = 4);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Text Utilities
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * Get text width in pixels
     */
    static int16_t getTextWidth(const char* text, uint8_t font);
    
    /**
     * Get text height in pixels
     */
    static int16_t getTextHeight(uint8_t font);
    
    /**
     * Truncate text to fit width
     */
    static String truncateText(const String& text, int16_t maxWidth,
                              uint8_t font, const char* ellipsis = "...");
    
    // ─────────────────────────────────────────────────────────────────────────
    // Screen Capture / Info
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * Get current rotation
     */
    static uint8_t getRotation();
    
    /**
     * Get screen width (accounts for rotation)
     */
    static int16_t getWidth();
    
    /**
     * Get screen height (accounts for rotation)
     */
    static int16_t getHeight();
    
    /**
     * Print display info to Serial
     */
    static void printInfo();

private:
    static TFT_eSPI* _tft;
    static bool _initialized;
    // volatile: written from loop() on Core 1 (screensaver dim/fade) and
    // from the NFC worker task on Core 0 (wakeScreen() -> backlightOn()).
    // Also guarded by _backlightMux below so the actual ledcWrite() can't
    // be interleaved between cores (that interleaving is what caused the
    // visible flicker at low dim levels).
    static volatile uint8_t _currentBrightness;
    static uint8_t _rotation;
    static portMUX_TYPE _backlightMux;
    
    // Private constructor (static class)
    TFTDisplayManager() = delete;
};

// ══════════════════════════════════════════════════════════════════════════════
// Convenience Macros
// ══════════════════════════════════════════════════════════════════════════════

// Quick access to TFT instance
#define TFT() TFTDisplayManager::getTFT()

// Quick color access
#define COLOR(name) TFTColors::name

#endif // TFT_DISPLAY_MANAGER_H