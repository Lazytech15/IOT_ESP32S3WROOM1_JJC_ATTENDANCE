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

static int  _checkIns    = 0;
static int  _checkOuts   = 0;
static bool _colonVisible = true;

// ── Last scan state ───────────────────────────────────────────────────────────
static String _lastInName  = "";
static String _lastInTime  = "--:--";
static String _lastOutName = "";
static String _lastOutTime = "--:--";

// ── Change notice (toast) state ─────────────────────────────────────────────
static bool     _lastWifiOk   = false;
static bool     _lastSdOk     = false;
static bool     _lastNfcOk    = false;
static bool     _noticeActive = false;
static uint32_t _noticeShownAt = 0;
static const uint32_t NOTICE_DURATION_MS = 5000;

// ── Z8 layout ─────────────────────────────────────────────────────────────────
// Z8_H = 64px total
//   4px  — label row top padding
//   10px — "CLOCK IN" label text
//   22px — name strip (green)
//   4px  — gap between rows
//   10px — "CLOCK OUT" label text
//   22px — name strip (red)
//   4px  — bottom padding  (4+10+22+4+10+22+4 = 76 → adjust Z8_H in .h to 76)
//
// rowY positions (relative to Z8_Y):
#define Z8_LABEL_IN_Y   (Z8_Y + 2)
#define Z8_STRIP_IN_Y   (Z8_Y + 13)
#define Z8_STRIP_H      22
#define Z8_LABEL_OUT_Y  (Z8_Y + 38)
#define Z8_STRIP_OUT_Y  (Z8_Y + 49)

// ══════════════════════════════════════════════════════════════════════════════
// Internal helpers
// ══════════════════════════════════════════════════════════════════════════════
static TFT_eSPI* tft() { return TFTDisplayManager::getTFT(); }

static void drawWireframeCard(int x, int y, int w, int h,
                               uint16_t accentCol, const char* label, int value) {
    TFT_eSPI* t = tft();
    if (!t) return;
    t->fillRect(x, y, w, h, TFTColors::BG_DARK);
    int c = 8;
    t->drawFastHLine(x,         y,         c, accentCol);
    t->drawFastVLine(x,         y,         c, accentCol);
    t->drawFastHLine(x+w-c,     y,         c, accentCol);
    t->drawFastVLine(x+w-1,     y,         c, accentCol);
    t->drawFastHLine(x,         y+h-1,     c, accentCol);
    t->drawFastVLine(x,         y+h-c,     c, accentCol);
    t->drawFastHLine(x+w-c,     y+h-1,     c, accentCol);
    t->drawFastVLine(x+w-1,     y+h-c,     c, accentCol);

    t->setTextColor(accentCol, TFTColors::BG_DARK);
    t->setTextDatum(TC_DATUM);
    t->drawString(label, x + w/2, y + 4, 1);

    char buf[8];
    snprintf(buf, sizeof(buf), "%03d", value);
    t->setTextColor(TFTColors::WHITE, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString(buf, x + w/2, y + h/2 + 6, 4);
    t->setTextDatum(TL_DATUM);
}

// ── drawNameStrip ─────────────────────────────────────────────────────────────
// Draws a 22px tall colored strip with the name inside.
// Uses font 2 (smaller than font 4) so full names fit without truncation.
// Falls back to font 1 if font 2 still overflows.
static void drawNameStrip(int stripY, uint16_t stripCol,
                           const String& name, const String& timeStr) {
    TFT_eSPI* t = tft();
    if (!t) return;

    bool isIdle = (name.length() == 0 || name == "---");

    if (isIdle) {
        t->fillRect(0, stripY, SCREEN_W, Z8_STRIP_H, TFTColors::BG_DARK);
        t->fillRect(0, stripY, 3, Z8_STRIP_H, stripCol);  // left accent bar
        // no text in idle strip — label row above already shows type
    } else {
        t->fillRect(0, stripY, SCREEN_W, Z8_STRIP_H, stripCol);

        // Time — right-aligned, small, dark
        t->setTextColor(TFTColors::BLACK, stripCol);
        t->setTextDatum(MR_DATUM);
        t->drawString(timeStr, SCREEN_W - 4, stripY + Z8_STRIP_H/2, 1);

        // Name — bold font 2, centered, truncated if needed
        String n = name;
        n.toUpperCase();

        // Try font 2 first (medium bold), fall back to font 1 if too wide
        int font = 2;
        int maxW = SCREEN_W - t->textWidth(timeStr, 1) - 10;
        while (n.length() > 0 && t->textWidth(n, font) > maxW) {
            if (font == 2 && t->textWidth(n, 1) <= maxW) {
                font = 1; break;
            }
            n.remove(n.length() - 1);
        }

        t->setTextColor(TFTColors::BLACK, stripCol);
        t->setTextDatum(ML_DATUM);
        t->drawString(n, 5, stripY + Z8_STRIP_H/2, font);
    }
    t->setTextDatum(TL_DATUM);
}

// ── drawZ8 ────────────────────────────────────────────────────────────────────
// Redraws the full Z8 zone: label + strip for clock-in, then clock-out.
static void drawZ8() {
    TFT_eSPI* t = tft();
    if (!t) return;

    // Clear entire Z8 zone
    t->fillRect(0, Z8_Y, SCREEN_W, Z8_H, TFTColors::BG_DARK);

    // ── Clock In label ────────────────────────────────────────────────────────
    t->setTextColor(TFTColors::SUCCESS, TFTColors::BG_DARK);
    t->setTextDatum(TL_DATUM);
    t->drawString("CLOCK IN", 5, Z8_LABEL_IN_Y, 1);

    // ── Clock In strip ────────────────────────────────────────────────────────
    drawNameStrip(Z8_STRIP_IN_Y, TFTColors::SUCCESS, _lastInName, _lastInTime);

    // ── Clock Out label ───────────────────────────────────────────────────────
    t->setTextColor(TFTColors::RED, TFTColors::BG_DARK);
    t->setTextDatum(TL_DATUM);
    t->drawString("CLOCK OUT", 5, Z8_LABEL_OUT_Y, 1);

    // ── Clock Out strip ───────────────────────────────────────────────────────
    drawNameStrip(Z8_STRIP_OUT_Y, TFTColors::RED, _lastOutName, _lastOutTime);
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

    static String   bootLogs[14];
    static uint16_t bootAddrs[14];
    static int      bootLogCount = 0;

    if (progress == 0) bootLogCount = 0;

    if (bootLogCount < 13) {
        bootLogs[bootLogCount]  = String(message);
        bootAddrs[bootLogCount] = 0x8000 + (progress * 0x14);
        bootLogCount++;
    } else {
        for (int i = 0; i < 12; i++) {
            bootLogs[i]  = bootLogs[i+1];
            bootAddrs[i] = bootAddrs[i+1];
        }
        bootLogs[12]  = String(message);
        bootAddrs[12] = 0x8000 + (progress * 0x14);
    }

    t->fillScreen(TFTColors::BLACK);
    t->setTextColor(TFTColors::WHITE, TFTColors::BLACK);
    t->setTextDatum(TL_DATUM);
    t->drawString("JJC-OS v4.2.0-esp32s3-wroom", 4,  4, 1);
    t->drawString("CPU: 240MHz  Mem: 8192K",       4, 14, 1);
    t->drawString("Boot sequence initiated...",     4, 24, 1);
    t->drawFastHLine(0, 36, SCREEN_W, TFTColors::BORDER_DIM);

    int y = 42;
    for (int i = 0; i < bootLogCount; i++) {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "[%04X] ", bootAddrs[i]);
        String   line = bootLogs[i];
        uint16_t col;
        if      (line.indexOf("FAIL")  >= 0 || line.indexOf("ERROR") >= 0) col = TFTColors::RED;
        else if (line.indexOf("...")   >= 0 || line.indexOf("ing")   >  0 ||
                 line.indexOf("Init") >= 0)                                col = TFTColors::TEXT_DIM;
        else                                                                col = TFTColors::SUCCESS;
        t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BLACK);
        t->drawString(prefix, 4, y, 2);
        t->setTextColor(col, TFTColors::BLACK);
        t->drawString(line, 55, y, 2);
        y += 18;
    }
    y += 8;
    t->setTextColor(TFTColors::SUCCESS, TFTColors::BLACK);
    t->drawString(progress >= 100 ? "root@jjc-sys:~# boot_complete"
                                  : "root@jjc-sys:~# _", 4, y, 2);
}

void drawStaticUI() {
    TFT_eSPI* t = tft();
    if (!t) return;
    t->fillScreen(TFTColors::BG_DARK);

    // ── Z1: Single-line header ─────────────────────────────────────────────────
    // "JJC ENGINEERING WORKS & GENERAL SERVICES" on one line using font 1
    t->fillRect(0, Z1_Y, SCREEN_W, Z1_H, TFTColors::BG_DARK);
    t->drawFastHLine(0, Z1_Y + Z1_H - 1, SCREEN_W, TFTColors::BORDER_DIM);
    t->setTextColor(TFTColors::ACCENT_CYAN, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString("JJC ENGINEERING WORKS &", SCREEN_W / 2, Z1_Y + 11, 1);
    t->drawString("GENERAL SERVICES",        SCREEN_W / 2, Z1_Y + 24, 1);
    // NOTE: Two lines are kept because the full string overflows 240px at font 1.
    // "JJC ENGINEERING WORKS &" is line 1, "GENERAL SERVICES" is line 2 —
    // both are horizontally centered and perfectly aligned.

    // ── Z3: HUD clock targeting box ───────────────────────────────────────────
    t->drawFastHLine(20,            Z3_Y,              30, TFTColors::TEXT_DIM);
    t->drawFastVLine(20,            Z3_Y,              20, TFTColors::TEXT_DIM);
    t->drawFastHLine(SCREEN_W - 50, Z3_Y,              30, TFTColors::TEXT_DIM);
    t->drawFastVLine(SCREEN_W - 21, Z3_Y,              20, TFTColors::TEXT_DIM);
    t->drawFastHLine(20,            Z3_Y + Z3_H - 1,   30, TFTColors::TEXT_DIM);
    t->drawFastVLine(20,            Z3_Y + Z3_H - 20,  20, TFTColors::TEXT_DIM);
    t->drawFastHLine(SCREEN_W - 50, Z3_Y + Z3_H - 1,   30, TFTColors::TEXT_DIM);
    t->drawFastVLine(SCREEN_W - 21, Z3_Y + Z3_H - 20,  20, TFTColors::TEXT_DIM);

    t->setTextColor(TFTColors::SUCCESS, TFTColors::BG_DARK);
    t->setTextDatum(TC_DATUM);
    t->drawString("24H LOCAL TIME", SCREEN_W / 2, Z3_Y + 4, 1);
    t->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString("AWAITING SYNC", SCREEN_W / 2, Z3_CLOCK_CY, 4);
    t->drawString("HRS", SCREEN_W / 2, Z3_HRS_Y, 1);

    // ── Z5: Divider ───────────────────────────────────────────────────────────
    for (int i = 0; i < SCREEN_W; i += 10)
        t->drawFastHLine(i, Z5_Y + 2, 5, TFTColors::BORDER_DIM);

    // ── Z6: Stat cards ────────────────────────────────────────────────────────
    drawWireframeCard(STAT_CARD_X1, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H,
                      TFTColors::SUCCESS, "CLOCK IN",  _checkIns);
    drawWireframeCard(STAT_CARD_X2, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H,
                      TFTColors::RED,     "CLOCK OUT", _checkOuts);

    // ── Z7: Divider ───────────────────────────────────────────────────────────
    for (int i = 0; i < SCREEN_W; i += 10)
        t->drawFastHLine(i, Z7_Y + 2, 5, TFTColors::BORDER_DIM);

    // ── Z8: Clock-In / Clock-Out name rows ────────────────────────────────────
    drawZ8();

    t->setTextDatum(TL_DATUM);
}

// ── 24H Military Clock ────────────────────────────────────────────────────────
// Redraws ONLY the digit row every tick — the borders, "HRS"/"24H LOCAL TIME"
// labels, and corner brackets are drawn once in drawStaticUI() and never
// touched again here.
//
// No separate fillRect() before drawString(): a full-width fillRect() followed
// by a second, separate drawString() call is two back-to-back SPI writes with
// a visible all-black gap between them, which is what caused the flicker (a
// black flash across the whole clock row every second). Font 7 is fixed-pitch
// and "HH:MM:SS" is always exactly 8 characters, so drawString() with a
// background colour erases the previous digits in place, in a single pass —
// the same technique the screensaver's clock already uses without flicker.
void updateClock(uint8_t h, uint8_t m, uint8_t s) {
    TFT_eSPI* t = tft();
    if (!t) return;

    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);

    t->setTextColor(TFTColors::WHITE, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString(buf, SCREEN_W / 2, Z3_CLOCK_CY, 7);

    t->setTextDatum(TL_DATUM);
}

// Clock has never been NTP/RTC-synced (clkEpoch == 0 in main.cpp): show a
// dashed placeholder instead of the free-running software clock, which
// otherwise counts up from 00:00:00 like a stopwatch and can be mistaken
// for a real (wrong) time. Same "--:--:--" convention already used by
// updateDate()'s unsynced fallback and the Z8 last-scan strips.
void updateClockUnsynced() {
    TFT_eSPI* t = tft();
    if (!t) return;

    t->setTextColor(TFTColors::WHITE, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    t->drawString("--:--:--", SCREEN_W / 2, Z3_CLOCK_CY, 7);

    t->setTextDatum(TL_DATUM);
}

void updateDate(const String& dateStr) {
    TFT_eSPI* t = tft();
    if (!t) return;
    t->fillRect(0, Z4_Y, SCREEN_W, Z4_H, TFTColors::BG_DARK);
    t->setTextColor(TFTColors::ACCENT_CYAN, TFTColors::BG_DARK);
    t->setTextDatum(MC_DATUM);
    String display = dateStr.length() > 0 ? dateStr : "DATE // --:--:--";
    t->drawString(display, SCREEN_W / 2, Z4_Y + 12, 2);
    t->setTextDatum(TL_DATUM);
}

void updateAttendanceStats(int checkIns, int checkOuts) {
    _checkIns  = checkIns;
    _checkOuts = checkOuts;
    drawWireframeCard(STAT_CARD_X1, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H,
                      TFTColors::SUCCESS, "CLOCK IN",  _checkIns);
    drawWireframeCard(STAT_CARD_X2, STAT_CARD_Y, STAT_CARD_W, STAT_CARD_H,
                      TFTColors::RED,     "CLOCK OUT", _checkOuts);
}

// ── updateLastScan ────────────────────────────────────────────────────────────
void updateLastScan(const String& name, const String& eventType,
                    const String& timeStr) {
    if (eventType == "check-in") {
        _lastInName = name;
        _lastInTime = timeStr;
    } else if (eventType == "check-out") {
        _lastOutName = name;
        _lastOutTime = timeStr;
    }
    drawZ8();
}

// ── clearLastScan ─────────────────────────────────────────────────────────────
// Resets the Clock-In/Clock-Out name strip back to blank. Needed because
// updateLastScan() only ever fires from an actual local NFC tap — if the
// admin deletes that attendance row from the portal afterwards, nothing
// else touches _lastInName/_lastOutName, so the deleted person's name would
// otherwise stay on screen forever even though the counts above it update.
void clearLastScan(const String& eventType) {
    if (eventType == "check-in" || eventType == "both") {
        _lastInName = "";
        _lastInTime = "--:--";
    }
    if (eventType == "check-out" || eventType == "both") {
        _lastOutName = "";
        _lastOutTime = "--:--";
    }
    drawZ8();
}

// ── updateStatusDots ──────────────────────────────────────────────────────────
void updateStatusDots(bool wifiOk, bool sdOk, bool nfcOk) {
    // Always remember the latest values, even while a notice is covering the
    // row — tickChangeNotice() uses these to repaint the real status once
    // the notice times out, instead of whatever was current when it opened.
    _lastWifiOk = wifiOk;
    _lastSdOk   = sdOk;
    _lastNfcOk  = nfcOk;

    if (_noticeActive) return;  // don't paint over an active toast

    TFT_eSPI* t = tft();
    if (!t) return;
    t->fillRect(0, Z2_Y, SCREEN_W, Z2_H, TFTColors::BG_DARK);
    int step = SCREEN_W / 3;
    t->setTextDatum(MC_DATUM);
    t->setTextColor(wifiOk ? TFTColors::SUCCESS : TFTColors::ERROR, TFTColors::BG_DARK);
    t->drawString(wifiOk ? "[NET:OK]" : "[NET:OFF]",  step/2,             Z2_Y + 10, 1);
    t->setTextColor(sdOk ? TFTColors::SUCCESS : TFTColors::ACCENT_ORANGE, TFTColors::BG_DARK);
    t->drawString(sdOk ? "[MEM:OK]" : "[MEM:ERR]",    step + step/2,      Z2_Y + 10, 1);
    t->setTextColor(nfcOk ? TFTColors::SUCCESS : TFTColors::ERROR, TFTColors::BG_DARK);
    t->drawString(nfcOk ? "[NFC:RDY]" : "[NFC:FLT]", (step*2) + step/2,  Z2_Y + 10, 1);
    t->setTextDatum(TL_DATUM);
}

void pulseStatus(bool state) {
    _colonVisible = state;
}

// ── showChangeNotice ────────────────────────────────────────────────────────
// Paints a 5-second toast over the status-dots row (Z2) to flag that
// something on the dashboard just changed for a reason OTHER than a live
// NFC tap (portal delete/edit, server-side reconciliation, etc). Auto-clears
// via tickChangeNotice() — call that once per loop() iteration.
void showChangeNotice(const String& message) {
    TFT_eSPI* t = tft();
    if (!t) return;

    _noticeActive  = true;
    _noticeShownAt = millis();

    t->fillRect(0, Z2_Y, SCREEN_W, Z2_H, TFTColors::ACCENT_ORANGE);
    t->setTextDatum(MC_DATUM);
    t->setTextColor(TFTColors::BG_DARK, TFTColors::ACCENT_ORANGE);
    t->drawString(message, SCREEN_W / 2, Z2_Y + Z2_H / 2, 1);
    t->setTextDatum(TL_DATUM);
}

// ── tickChangeNotice ─────────────────────────────────────────────────────────
// Call once per loop() tick. No-op unless a notice is showing; once
// NOTICE_DURATION_MS has elapsed it restores the real status row.
void tickChangeNotice() {
    if (!_noticeActive) return;
    if (millis() - _noticeShownAt < NOTICE_DURATION_MS) return;

    _noticeActive = false;
    updateStatusDots(_lastWifiOk, _lastSdOk, _lastNfcOk);
}

TFT_eSPI* dashboardGetTFT() { return TFTDisplayManager::getTFT(); }