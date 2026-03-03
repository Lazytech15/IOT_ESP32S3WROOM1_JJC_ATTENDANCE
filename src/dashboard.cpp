// ══════════════════════════════════════════════════════════════════════════════
// dashboard.cpp  — Portrait Edition (240×320)
// Shows: large clock, date, check-in/out counters, last scan name
// ══════════════════════════════════════════════════════════════════════════════

#include "dashboard.h"
#include <math.h>

// ── Card geometry ──────────────────────────────────────────────────────────────
#define STAT_CARD_W    108
#define STAT_CARD_H     64
#define STAT_CARD_X1     6
#define STAT_CARD_X2   126
#define STAT_CARD_Y    (Z6_Y + 3)

// ── Cached counters so we can redraw without arguments ─────────────────────────
static int  _checkIns   = 0;
static int  _checkOuts  = 0;
static bool _colonVisible = true;

// ══════════════════════════════════════════════════════════════════════════════
// Internal helpers
// ══════════════════════════════════════════════════════════════════════════════
static TFT_eSPI* tft() { return TFTDisplayManager::getTFT(); }

static void drawStatCard(int x, int y, int w, int h,
                          uint16_t accentCol,
                          const char* label,
                          int value,
                          uint16_t labelCol) {
    TFT_eSPI* t = tft();
    if (!t) return;

    uint16_t bg = TFTColors::BG_LIGHT;
    t->fillRoundRect(x, y, w, h, 8, bg);
    t->fillRoundRect(x + 2, y + 2, 3, h - 4, 2, accentCol);
    t->drawRoundRect(x, y, w, h, 8, TFTColors::BORDER_DIM);

    // Label
    t->setTextColor(labelCol, bg);
    t->setTextDatum(TL_DATUM);
    t->setTextSize(1);
    t->drawString(label, x + 10, y + 8, 2);

    // Value — large centered number
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);
    t->setTextColor(TFTColors::TEXT_PRIMARY, bg);
    t->setTextDatum(MC_DATUM);
    t->drawString(buf, x + w / 2, y + h / 2 + 8, 6);
    t->setTextDatum(TL_DATUM);
}

// ══════════════════════════════════════════════════════════════════════════════
// dashboardInit
// ══════════════════════════════════════════════════════════════════════════════
void dashboardInit() {
    if (!TFTDisplayManager::isInitialized()) {
        Serial.println("[Dashboard] ERROR: TFTDisplayManager not initialized!");
        return;
    }
    TFTDisplayManager::clearScreen(TFTColors::BG_DARK);
    Serial.println("[Dashboard] Portrait dashboard ready (240x320)");
}

// ══════════════════════════════════════════════════════════════════════════════
// showLoadingAnimation  — portrait version
// ══════════════════════════════════════════════════════════════════════════════
void showLoadingAnimation(int progress, const char* message) {
    TFT_eSPI* t = tft();
    if (!t) return;

    progress = constrain(progress, 0, 100);

    // Background
    for (int y = 0; y < SCREEN_H; y++) {
        uint16_t c = t->color565(0, y / 10, y / 7);
        t->drawFastHLine(0, y, SCREEN_W, c);
    }

    // Logo circle
    int logoX = SCREEN_W / 2;
    int logoY = 100;
    unsigned long now = millis();
    float pulse = 1.0f + 0.12f * sinf(now / 350.0f);
    int r = (int)(38 * pulse);

    for (int i = 4; i > 0; i--)
        t->drawCircle(logoX, logoY, r + i * 2,
                      t->color565(0, 60 - i * 10, 120 - i * 15));
    t->fillCircle(logoX, logoY, r, TFTColors::BG_MID);
    t->drawCircle(logoX, logoY, r,     TFTColors::ACCENT_TEAL);
    t->drawCircle(logoX, logoY, r + 2, TFTColors::ACCENT_CYAN);

    // Spinner dot
    float angle = (now % 1500) / 1500.0f * 2.0f * PI;
    int sx = logoX + (int)(cosf(angle) * (r - 6));
    int sy = logoY + (int)(sinf(angle) * (r - 6));
    t->fillCircle(sx, sy, 4, TFTColors::ACCENT_CYAN);

    // Centre icon text
    t->setTextColor(TFTColors::ACCENT_TEAL);
    t->setTextDatum(MC_DATUM);
    t->drawString("NFC", logoX, logoY, 2);
    t->setTextDatum(TL_DATUM);

    // Title
    t->setTextColor(TFTColors::TEXT_PRIMARY);
    t->setTextDatum(MC_DATUM);
    t->drawString("JJC Attendance", SCREEN_W / 2, 170, 2);
    t->setTextColor(TFTColors::ACCENT_CYAN);
    t->drawString("ESP32-S3 WROOM-1", SCREEN_W / 2, 188, 1);
    t->setTextDatum(TL_DATUM);

    // Progress bar
    int barX = 20, barY = 210, barW = SCREEN_W - 40, barH = 18;
    t->fillRoundRect(barX, barY, barW, barH, 9, TFTColors::BG_MID);
    int fill = (barW - 4) * progress / 100;
    if (fill > 0) {
        uint16_t col = (progress < 33) ? TFTColors::ACCENT_TEAL
                     : (progress < 66) ? TFTColors::ACCENT_CYAN
                                       : TFTColors::SUCCESS;
        t->fillRoundRect(barX + 2, barY + 2, fill, barH - 4, 7, col);
    }
    t->drawRoundRect(barX, barY, barW, barH, 9, TFTColors::BORDER_BRIGHT);

    // Percentage
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", progress);
    t->setTextColor(TFTColors::TEXT_PRIMARY);
    t->setTextDatum(MC_DATUM);
    t->drawString(pct, SCREEN_W / 2, barY + barH / 2, 1);
    t->setTextDatum(TL_DATUM);

    // Status message
    t->fillRect(0, 238, SCREEN_W, 30, TFTColors::BG_DARK);
    t->setTextColor(TFTColors::TEXT_SECONDARY);
    t->setTextDatum(MC_DATUM);
    t->drawString(message, SCREEN_W / 2, 253, 1);
    t->setTextDatum(TL_DATUM);
}

// ══════════════════════════════════════════════════════════════════════════════
// drawStaticUI  — draws the complete portrait dashboard skeleton
// ══════════════════════════════════════════════════════════════════════════════
void drawStaticUI() {
    TFT_eSPI* t = tft();
    if (!t) return;

    t->fillScreen(TFTColors::BG_DARK);

    // ── Z1: Header ────────────────────────────────────────────────────────────
    t->fillRect(0, Z1_Y, SCREEN_W, Z1_H, TFTColors::BG_MID);
    // Accent stripe at bottom of header
    t->fillRect(0, Z1_Y + Z1_H - 3, SCREEN_W, 3, TFTColors::ACCENT_TEAL);

    t->setTextColor(TFTColors::TEXT_PRIMARY, TFTColors::BG_MID);
    t->setTextDatum(ML_DATUM);
    t->drawString("JJC Attendance", 10, Z1_Y + Z1_H / 2, 2);
    t->setTextDatum(TL_DATUM);

    // Status indicator dot (top-right) — will be updated by updateStatusDots
    t->fillCircle(SCREEN_W - 12, Z1_Y + Z1_H / 2, 5, TFTColors::ACCENT_TEAL);

    // ── Z2: Status bar ────────────────────────────────────────────────────────
    t->fillRect(0, Z2_Y, SCREEN_W, Z2_H, TFTColors::BG_DARK);
    updateStatusDots(false, false, false);  // placeholder — re-called from main

    // ── Z3: Clock area placeholder ────────────────────────────────────────────
    t->fillRect(0, Z3_Y, SCREEN_W, Z3_H, TFTColors::BG_DARK);
    t->setTextColor(TFTColors::ACCENT_TEAL, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString("--:--:--", SCREEN_W / 2, Z3_Y + Z3_H / 2 - 8, 7);
    t->setTextDatum(TL_DATUM);

    // ── Z4: Date placeholder ──────────────────────────────────────────────────
    t->fillRect(0, Z4_Y, SCREEN_W, Z4_H, TFTColors::BG_DARK);
    t->setTextColor(TFTColors::TEXT_SECONDARY, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString("Initializing...", SCREEN_W / 2, Z4_Y + 11, 1);
    t->setTextDatum(TL_DATUM);

    // ── Z5: Divider ───────────────────────────────────────────────────────────
    t->fillRect(0, Z5_Y, SCREEN_W, Z5_H, TFTColors::ACCENT_TEAL);

    // ── Z6: Stat cards ────────────────────────────────────────────────────────
    t->fillRect(0, Z6_Y, SCREEN_W, Z6_H, TFTColors::BG_DARK);
    drawStatCard(STAT_CARD_X1, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H,
                 TFTColors::ACCENT_TEAL, "CLOCK IN", _checkIns,
                 TFTColors::ACCENT_CYAN);
    drawStatCard(STAT_CARD_X2, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H,
                 TFTColors::ACCENT_ORANGE, "CLOCK OUT", _checkOuts,
                 TFTColors::ACCENT_CYAN);

    // ── Z7: Divider ───────────────────────────────────────────────────────────
    t->fillRect(0, Z7_Y, SCREEN_W, Z7_H, TFTColors::BORDER_BRIGHT);

    // ── Z8: Last scan placeholder ─────────────────────────────────────────────
    t->fillRect(0, Z8_Y, SCREEN_W, Z8_H, TFTColors::BG_MID);
    t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_MID);
    t->setTextDatum(MC_DATUM);
    t->drawString("No scans yet", SCREEN_W / 2, Z8_Y + Z8_H / 2, 2);
    t->setTextDatum(TL_DATUM);

    // ── Z9: SD info ───────────────────────────────────────────────────────────
    t->fillRect(0, Z9_Y, SCREEN_W, Z9_H, TFTColors::BG_DARK);
    t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
    t->setCursor(6, Z9_Y + 7);
    t->setTextSize(1);
    t->print("SD: --  |  Scan NFC card to log attendance");

    // ── Z10: Footer ───────────────────────────────────────────────────────────
    t->fillRect(0, Z10_Y, SCREEN_W, Z10_H, TFTColors::ACCENT_TEAL);

    Serial.println("[Dashboard] Static UI drawn (portrait 240x320)");
}

// ══════════════════════════════════════════════════════════════════════════════
// updateClock
// ══════════════════════════════════════════════════════════════════════════════
void updateClock(uint8_t h, uint8_t m, uint8_t s) {
    TFT_eSPI* t = tft();
    if (!t) return;

    t->fillRect(0, Z3_Y, SCREEN_W, Z3_H, TFTColors::BG_DARK);

    char buf[12];
    if (_colonVisible)
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    else
        snprintf(buf, sizeof(buf), "%02d %02d %02d", h, m, s);

    t->setTextColor(TFTColors::ACCENT_TEAL, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    // Font 7 is a large 7-segment style — use font 6 if 7 not available
    t->drawString(buf, SCREEN_W / 2, Z3_Y + Z3_H / 2, 7);
    t->setTextDatum(TL_DATUM);
}

// ══════════════════════════════════════════════════════════════════════════════
// updateDate
// ══════════════════════════════════════════════════════════════════════════════
void updateDate(const String& dateStr) {
    TFT_eSPI* t = tft();
    if (!t) return;

    t->fillRect(0, Z4_Y, SCREEN_W, Z4_H, TFTColors::BG_DARK);
    t->setTextColor(TFTColors::TEXT_SECONDARY, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString(dateStr, SCREEN_W / 2, Z4_Y + 11, 2);
    t->setTextDatum(TL_DATUM);
}

// ══════════════════════════════════════════════════════════════════════════════
// updateAttendanceStats
// ══════════════════════════════════════════════════════════════════════════════
void updateAttendanceStats(int checkIns, int checkOuts) {
    _checkIns  = checkIns;
    _checkOuts = checkOuts;

    TFT_eSPI* t = tft();
    if (!t) return;

    t->fillRect(0, Z6_Y, SCREEN_W, Z6_H, TFTColors::BG_DARK);
    drawStatCard(STAT_CARD_X1, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H,
                 TFTColors::ACCENT_TEAL, "CLOCK IN", _checkIns,
                 TFTColors::ACCENT_CYAN);
    drawStatCard(STAT_CARD_X2, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H,
                 TFTColors::ACCENT_ORANGE, "CLOCK OUT", _checkOuts,
                 TFTColors::ACCENT_CYAN);
}

// ══════════════════════════════════════════════════════════════════════════════
// updateLastScan
// ══════════════════════════════════════════════════════════════════════════════
void updateLastScan(const String& name,
                    const String& eventType,
                    const String& timeStr) {
    TFT_eSPI* t = tft();
    if (!t) return;

    t->fillRect(0, Z8_Y, SCREEN_W, Z8_H, TFTColors::BG_MID);

    // Event label
    bool isIn   = (eventType == "check-in");
    uint16_t ec = isIn ? TFTColors::ACCENT_TEAL : TFTColors::ACCENT_ORANGE;
    t->setTextColor(ec, TFTColors::BG_MID);
    t->setCursor(8, Z8_Y + 6);
    t->setTextSize(1);
    t->print(isIn ? "CHECK-IN" : "CHECK-OUT");

    // Time
    t->setTextColor(TFTColors::TEXT_PRIMARY, TFTColors::BG_MID);
    t->setTextDatum(MR_DATUM);
    t->drawString(timeStr, SCREEN_W - 6, Z8_Y + 12, 2);
    t->setTextDatum(TL_DATUM);

    // Divider
    t->drawFastHLine(6, Z8_Y + 22, SCREEN_W - 12, TFTColors::BORDER_DIM);

    // Employee name
    t->setTextColor(TFTColors::TEXT_PRIMARY, TFTColors::BG_MID);
    t->setTextDatum(MC_DATUM);

    // Truncate name to fit
    String n = name;
    while (n.length() > 0 && t->textWidth(n, 2) > SCREEN_W - 16)
        n.remove(n.length() - 1);
    if (n.length() < name.length()) n += "..";

    t->drawString(n, SCREEN_W / 2, Z8_Y + 38, 2);
    t->setTextDatum(TL_DATUM);
}

// ══════════════════════════════════════════════════════════════════════════════
// updateStatusDots
// ══════════════════════════════════════════════════════════════════════════════
void updateStatusDots(bool wifiOk, bool sdOk, bool nfcOk) {
    TFT_eSPI* t = tft();
    if (!t) return;

    t->fillRect(0, Z2_Y, SCREEN_W, Z2_H, TFTColors::BG_DARK);
    t->setTextSize(1);

    // WiFi
    uint16_t wc = wifiOk ? TFTColors::SUCCESS : TFTColors::ERROR;
    t->fillCircle(12, Z2_Y + 9, 4, wc);
    t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
    t->setCursor(20, Z2_Y + 5);
    t->print(wifiOk ? "WiFi" : "AP");

    // SD
    uint16_t sc = sdOk ? TFTColors::SUCCESS : TFTColors::WARNING;
    t->fillCircle(72, Z2_Y + 9, 4, sc);
    t->setCursor(80, Z2_Y + 5);
    t->print(sdOk ? "SD" : "NoSD");

    // NFC
    uint16_t nc = nfcOk ? TFTColors::SUCCESS : TFTColors::ERROR;
    t->fillCircle(122, Z2_Y + 9, 4, nc);
    t->setCursor(130, Z2_Y + 5);
    t->print("NFC");

    // Uptime placeholder (right-aligned)
    t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
    t->setTextDatum(MR_DATUM);
    t->drawString("ready", SCREEN_W - 4, Z2_Y + 9, 1);
    t->setTextDatum(TL_DATUM);
}

// ══════════════════════════════════════════════════════════════════════════════
// pulseStatus — blink the colon in the clock
// ══════════════════════════════════════════════════════════════════════════════
void pulseStatus(bool state) {
    _colonVisible = state;
    // Header status dot
    TFT_eSPI* t = tft();
    if (!t) return;
    uint16_t c = state ? TFTColors::SUCCESS : TFTColors::ACCENT_TEAL;
    t->fillCircle(SCREEN_W - 12, Z1_Y + Z1_H / 2, 5, c);
}

TFT_eSPI* dashboardGetTFT() { return TFTDisplayManager::getTFT(); }