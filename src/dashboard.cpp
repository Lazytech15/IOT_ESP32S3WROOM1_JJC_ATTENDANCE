// ══════════════════════════════════════════════════════════════════════════════
// dashboard.cpp  — Portrait Edition (240×320)
// UI/UX: Tactical HUD / Military 24H Telemetry (Terminal Boot Sequence)
// ══════════════════════════════════════════════════════════════════════════════

#include "dashboard.h"
#include <math.h>

// ── Wireframe Card Geometry ───────────────────────────────────────────────────
#define STAT_CARD_W    110
#define STAT_CARD_H     56
#define STAT_CARD_X1     6
#define STAT_CARD_X2   124
#define STAT_CARD_Y    (Z6_Y + 5)

static int  _checkIns   = 0;
static int  _checkOuts  = 0;
static bool _colonVisible = true;

// ══════════════════════════════════════════════════════════════════════════════
// Internal helpers
// ══════════════════════════════════════════════════════════════════════════════
static TFT_eSPI* tft() { return TFTDisplayManager::getTFT(); }

// Draws a military/HUD style wireframe bracket card
static void drawWireframeCard(int x, int y, int w, int h, uint16_t accentCol, const char* label, int value) {
    TFT_eSPI* t = tft();
    if (!t) return;

    // Clear background
    t->fillRect(x, y, w, h, TFTColors::BG_DARK);

    // Draw HUD Corner brackets instead of full borders
    int corner = 8;
    // Top Left
    t->drawFastHLine(x, y, corner, accentCol);
    t->drawFastVLine(x, y, corner, accentCol);
    // Top Right
    t->drawFastHLine(x + w - corner, y, corner, accentCol);
    t->drawFastVLine(x + w - 1, y, corner, accentCol);
    // Bottom Left
    t->drawFastHLine(x, y + h - 1, corner, accentCol);
    t->drawFastVLine(x, y + h - corner, corner, accentCol);
    // Bottom Right
    t->drawFastHLine(x + w - corner, y + h - 1, corner, accentCol);
    t->drawFastVLine(x + w - 1, y + h - corner, corner, accentCol);

    // Top Label (Tactical size)
    t->setTextColor(accentCol, TFTColors::BG_DARK);
    t->setTextDatum(TC_DATUM);
    t->drawString(label, x + w / 2, y + 4, 1);

    // Value (Large)
    char buf[8];
    snprintf(buf, sizeof(buf), "%03d", value); // 3-digit zero pad for military look
    t->setTextColor(TFTColors::WHITE, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString(buf, x + w / 2, y + h / 2 + 6, 4);
    t->setTextDatum(TL_DATUM);
}

// ══════════════════════════════════════════════════════════════════════════════
// Core API
// ══════════════════════════════════════════════════════════════════════════════
void dashboardInit() {
    if (!TFTDisplayManager::isInitialized()) return;
    TFTDisplayManager::clearScreen(TFTColors::BG_DARK);
}

// ── Linux Terminal Style Boot Sequence ────────────────────────────────────────
void showLoadingAnimation(int progress, const char* message) {
    TFT_eSPI* t = tft();
    if (!t) return;
    progress = constrain(progress, 0, 100);

    // Static variables preserve the log history across function calls
    static String bootLogs[14];
    static uint16_t bootAddrs[14];
    static int bootLogCount = 0;

    // Reset log on start
    if (progress == 0) {
        bootLogCount = 0;
    }

    // Shift logs up if we reach the bottom of the screen
    if (bootLogCount < 13) {
        bootLogs[bootLogCount] = String(message);
        bootAddrs[bootLogCount] = 0x8000 + (progress * 0x14); // Fake memory address logic
        bootLogCount++;
    } else {
        for (int i = 0; i < 12; i++) {
            bootLogs[i] = bootLogs[i+1];
            bootAddrs[i] = bootAddrs[i+1];
        }
        bootLogs[12] = String(message);
        bootAddrs[12] = 0x8000 + (progress * 0x14);
    }

    // Pure black background for terminal feel
    t->fillScreen(TFTColors::BLACK);

    // BIOS / Kernel Header (Tiny font 1)
    t->setTextColor(TFTColors::WHITE, TFTColors::BLACK);
    t->setTextDatum(TL_DATUM);
    t->drawString("JJC-OS v4.2.0-esp32s3-wroom", 4, 4, 1);
    t->drawString("CPU: 240MHz  Mem: 8192K", 4, 14, 1);
    t->drawString("Boot sequence initiated...", 4, 24, 1);
    t->drawFastHLine(0, 36, SCREEN_W, TFTColors::BORDER_DIM);

    // Print Log Lines
    int y = 42;
    for (int i = 0; i < bootLogCount; i++) {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "[%04X] ", bootAddrs[i]);
        
        String line = bootLogs[i];
        uint16_t col = TFTColors::ACCENT_CYAN;
        
        // Color code based on keyword
        if (line.indexOf("FAIL") >= 0 || line.indexOf("ERROR") >= 0) {
            col = TFTColors::RED;
        } else if (line.indexOf("...") >= 0 || line.indexOf("ing") > 0 || line.indexOf("Init") >= 0) {
            col = TFTColors::TEXT_DIM; // Gray/dim for active tasks
        } else {
            col = TFTColors::SUCCESS; // Green for completed/OK
        }

        // Draw Memory Address Prefix (Dimmed)
        t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BLACK);
        t->drawString(prefix, 4, y, 2); // Font 2 for standard terminal font
        
        // Draw Actual Status Message
        t->setTextColor(col, TFTColors::BLACK);
        t->drawString(line, 55, y, 2);
        
        y += 18;
    }

    // Fake Command Prompt at the bottom
    y += 8;
    t->setTextColor(TFTColors::SUCCESS, TFTColors::BLACK);
    if (progress >= 100) {
        t->drawString("root@jjc-sys:~# boot_complete", 4, y, 2);
    } else {
        t->drawString("root@jjc-sys:~# _", 4, y, 2);
    }
}

void drawStaticUI() {
    TFT_eSPI* t = tft();
    if (!t) return;
    t->fillScreen(TFTColors::BG_DARK);

    // ── Z1: Telemetry Header (Now with JJC text) ──────────────────────────────
    t->fillRect(0, Z1_Y, SCREEN_W, Z1_H, TFTColors::BG_DARK);
    t->drawFastHLine(0, Z1_Y + Z1_H - 1, SCREEN_W, TFTColors::BORDER_DIM);
    
    t->setTextColor(TFTColors::ACCENT_CYAN, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    // Two-line tactical header
    t->drawString("JJC ENGINEERING &", SCREEN_W / 2, Z1_Y + 12, 1);
    t->drawString("GENERAL SERVICES",  SCREEN_W / 2, Z1_Y + 24, 1);
    
    // ── Z2: Status Badges ─────────────────────────────────────────────────────
    updateStatusDots(false, false, false); 

    // ── Z3/Z4: Setup Military Clock Box ───────────────────────────────────────
    // Draw the permanent HUD targeting box around where the clock will be
    t->drawFastHLine(20, Z3_Y, 30, TFTColors::TEXT_DIM);
    t->drawFastVLine(20, Z3_Y, 20, TFTColors::TEXT_DIM);
    
    t->drawFastHLine(SCREEN_W - 50, Z3_Y, 30, TFTColors::TEXT_DIM);
    t->drawFastVLine(SCREEN_W - 21, Z3_Y, 20, TFTColors::TEXT_DIM);
    
    t->drawFastHLine(20, Z3_Y + Z3_H - 1, 30, TFTColors::TEXT_DIM);
    t->drawFastVLine(20, Z3_Y + Z3_H - 20, 20, TFTColors::TEXT_DIM);
    
    t->drawFastHLine(SCREEN_W - 50, Z3_Y + Z3_H - 1, 30, TFTColors::TEXT_DIM);
    t->drawFastVLine(SCREEN_W - 21, Z3_Y + Z3_H - 20, 20, TFTColors::TEXT_DIM);

    // Top indicator
    t->setTextColor(TFTColors::SUCCESS, TFTColors::BG_DARK);
    t->setTextDatum(TC_DATUM);
    t->drawString("24H LOCAL TIME", SCREEN_W / 2, Z3_Y + 4, 1);

    t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
    t->drawString("AWAITING SYNC", SCREEN_W / 2, Z3_Y + Z3_H / 2 - 10, 4);

    // ── Z5: Divider ───────────────────────────────────────────────────────────
    for(int i = 0; i < SCREEN_W; i += 10) t->drawFastHLine(i, Z5_Y + 2, 5, TFTColors::BORDER_DIM);

    // ── Z6: Wireframe Stats (UPDATED COLORS/LABELS) ───────────────────────────
    drawWireframeCard(STAT_CARD_X1, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H, TFTColors::SUCCESS, "CLOCK IN", _checkIns);
    drawWireframeCard(STAT_CARD_X2, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H, TFTColors::RED, "CLOCK OUT", _checkOuts);

    // ── Z7: Divider ───────────────────────────────────────────────────────────
    for(int i = 0; i < SCREEN_W; i += 10) t->drawFastHLine(i, Z7_Y + 2, 5, TFTColors::BORDER_DIM);

    // ── Z8: Terminal Output ───────────────────────────────────────────────────
    updateLastScan("AWAITING_INPUT...", "", "--:--");

    // ── Z9: Footer ────────────────────────────────────────────────────────────
    t->fillRect(0, Z9_Y, SCREEN_W, Z9_H, TFTColors::ACCENT_TEAL);
    t->setTextColor(TFTColors::BLACK, TFTColors::ACCENT_TEAL);
    t->setTextDatum(MC_DATUM);
    t->drawString("SYSTEM ARMED // SCAN RFID", SCREEN_W / 2, Z9_Y + Z9_H / 2, 1);
    t->setTextDatum(TL_DATUM);
}

// The core 24H Military Clock render
void updateClock(uint8_t h, uint8_t m, uint8_t s) {
    TFT_eSPI* t = tft();
    if (!t) return;
    
    // Clear only the inside of the targeting box
    t->fillRect(22, Z3_Y + 16, SCREEN_W - 44, Z3_H - 18, TFTColors::BG_DARK);
    
    char buf[12];
    
    // FIX: Always use colons! This keeps the text width constant and stops the jitter/double-text bug.
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    
    t->setTextColor(TFTColors::WHITE, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    
    // Swapped from Font 7 to Font 4 to guarantee it renders on all screens
    t->drawString(buf, SCREEN_W / 2, Z3_Y + Z3_H / 2 + 5, 4); 
    
    // Military "ZULU / HRS" visual cue
    t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
    t->drawString("HRS", SCREEN_W - 35, Z3_Y + Z3_H / 2 + 10, 1);
    
    t->setTextDatum(TL_DATUM);
}

void updateDate(const String& dateStr) {
    TFT_eSPI* t = tft();
    if (!t) return;
    t->fillRect(0, Z4_Y, SCREEN_W, Z4_H, TFTColors::BG_DARK);
    t->setTextColor(TFTColors::ACCENT_CYAN, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);

    // dateStr is already formatted as "TUE // 2026-03-03" from buildDateStr()
    // but we keep a fallback for robustness
    String display = dateStr.length() > 0 ? dateStr : "DATE // --:--:--";
    t->drawString(display, SCREEN_W / 2, Z4_Y + 12, 2);
    t->setTextDatum(TL_DATUM);
}

void updateAttendanceStats(int checkIns, int checkOuts) {
    _checkIns  = checkIns;
    _checkOuts = checkOuts;
    drawWireframeCard(STAT_CARD_X1, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H, TFTColors::SUCCESS, "CLOCK IN", _checkIns);
    drawWireframeCard(STAT_CARD_X2, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H, TFTColors::RED, "CLOCK OUT", _checkOuts);
}

// "Terminal Log" style recent scan
void updateLastScan(const String& name, const String& eventType, const String& timeStr) {
    TFT_eSPI* t = tft();
    if (!t) return;

    t->fillRect(0, Z8_Y, SCREEN_W, Z8_H, TFTColors::BG_DARK);

    bool isIn = (eventType == "check-in");
    uint16_t ec = isIn ? TFTColors::SUCCESS : TFTColors::RED; // Changed to pure RED

    t->setTextColor(ec, TFTColors::BG_DARK);
    t->setCursor(10, Z8_Y + 10);
    t->setTextSize(1);
    if(eventType != "") t->print(isIn ? "> SYS.INBOUND: " : "> SYS.OUTBOUND:");
    else t->print("> SYS.IDLE");

    t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
    t->setTextDatum(TR_DATUM);
    t->drawString(timeStr, SCREEN_W - 10, Z8_Y + 10, 1);

    t->setTextColor(TFTColors::WHITE, TFTColors::BG_DARK);
    t->setTextDatum(ML_DATUM);
    
    String n = name;
    n.toUpperCase(); // Military uppercase enforcement
    while (n.length() > 0 && t->textWidth(n, 2) > SCREEN_W - 20) n.remove(n.length() - 1);
    
    t->drawString(n, 10, Z8_Y + 36, 2);
    t->setTextDatum(TL_DATUM);
}

// Tactical readout for connections
void updateStatusDots(bool wifiOk, bool sdOk, bool nfcOk) {
    TFT_eSPI* t = tft();
    if (!t) return;
    t->fillRect(0, Z2_Y, SCREEN_W, Z2_H, TFTColors::BG_DARK);

    int step = SCREEN_W / 3;
    
    t->setTextColor(wifiOk ? TFTColors::SUCCESS : TFTColors::ERROR, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString(wifiOk ? "[NET:OK]" : "[NET:OFF]", step/2, Z2_Y + 10, 1);

    t->setTextColor(sdOk ? TFTColors::SUCCESS : TFTColors::ACCENT_ORANGE, TFTColors::BG_DARK);
    t->drawString(sdOk ? "[MEM:OK]" : "[MEM:ERR]", step + step/2, Z2_Y + 10, 1);

    t->setTextColor(nfcOk ? TFTColors::SUCCESS : TFTColors::ERROR, TFTColors::BG_DARK);
    t->drawString(nfcOk ? "[NFC:RDY]" : "[NFC:FLT]", (step*2) + step/2, Z2_Y + 10, 1);
    
    t->setTextDatum(TL_DATUM);
}

void pulseStatus(bool state) {
    _colonVisible = state;
}

TFT_eSPI* dashboardGetTFT() { return TFTDisplayManager::getTFT(); }