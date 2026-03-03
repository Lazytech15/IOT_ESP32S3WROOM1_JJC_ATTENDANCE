// ══════════════════════════════════════════════════════════════════════════════
// dashboard.h  — Portrait Edition (240×320)
// Dashboard UI — clock + attendance counters only
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "TFTDisplayManager.h"

// ── Screen geometry (portrait) ─────────────────────────────────────────────────
#define SCREEN_W   240
#define SCREEN_H   320

// ══════════════════════════════════════════════════════════════════════════════
// Zone layout (Y, H) — portrait 240×320
// ══════════════════════════════════════════════════════════════════════════════
//  Z1  Header bar         y=  0, h= 36   "JJC Attendance"
//  Z2  Status bar         y= 36, h= 18   WiFi • SD • NFC dots
//  Z3  Clock area         y= 54, h= 90   HH:MM:SS large
//  Z4  Date area          y=144, h= 22   Day, Month DD YYYY
//  Z5  Divider            y=166, h=  2
//  Z6  Stat card row      y=168, h= 70   [ CLOCK IN  ]  [ CLOCK OUT ]
//  Z7  Divider            y=238, h=  2
//  Z8  Last scan          y=240, h= 52   Employee name + time
//  Z9  SD info            y=292, h= 24   SD free space
//  Z10 Footer             y=318, h=  2

#define Z1_Y    0
#define Z1_H   36
#define Z2_Y   36
#define Z2_H   18
#define Z3_Y   54
#define Z3_H   90
#define Z4_Y  144
#define Z4_H   22
#define Z5_Y  166
#define Z5_H    2
#define Z6_Y  168
#define Z6_H   70
#define Z7_Y  238
#define Z7_H    2
#define Z8_Y  240
#define Z8_H   52
#define Z9_Y  292
#define Z9_H   24
#define Z10_Y 318
#define Z10_H   2

// ══════════════════════════════════════════════════════════════════════════════
// API
// ══════════════════════════════════════════════════════════════════════════════

/** Init dashboard. TFTDisplayManager::init() MUST be called first. */
void dashboardInit();

/** Loading animation (portrait). */
void showLoadingAnimation(int progress, const char* message);

/** Draw full static UI layout. */
void drawStaticUI();

/**
 * Update the large clock display.
 * @param h hours 0-23, @param m minutes, @param s seconds
 * Call every second.
 */
void updateClock(uint8_t h, uint8_t m, uint8_t s);

/**
 * Update the date string below the clock.
 * @param dateStr e.g. "Mon, Jan 15 2025"
 */
void updateDate(const String& dateStr);

/**
 * Update the stat counters (attendance cards).
 * @param checkIns  total check-ins today
 * @param checkOuts total check-outs today
 */
void updateAttendanceStats(int checkIns, int checkOuts);

/**
 * Update "last scan" footer row.
 * @param name       Employee full name (truncated to fit)
 * @param eventType  "check-in" or "check-out"
 * @param timeStr    "HH:MM"
 */
void updateLastScan(const String& name,
                    const String& eventType,
                    const String& timeStr);

/**
 * Update status indicator dots in status bar.
 * @param wifiOk  true = WiFi connected
 * @param sdOk    true = SD card ready
 * @param nfcOk   true = NFC ready
 */
void updateStatusDots(bool wifiOk, bool sdOk, bool nfcOk);

/** Toggle the blinking colon in the clock. */
void pulseStatus(bool state);

/** Get pointer to underlying TFT instance. */
TFT_eSPI* dashboardGetTFT();