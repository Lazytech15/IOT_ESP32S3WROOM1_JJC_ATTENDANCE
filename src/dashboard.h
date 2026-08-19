// ══════════════════════════════════════════════════════════════════════════════
// dashboard.h  — Portrait Edition (240×320)
// UI/UX: Tactical HUD / Military 24H Telemetry
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "TFTDisplayManager.h"

#define SCREEN_W   240
#define SCREEN_H   320

// ══════════════════════════════════════════════════════════════════════════════
// Zone layout (Y, H) — portrait 240×320
//
// Z8 breakdown (76px):
//   2px  padding
//   10px CLOCK IN label
//   22px green name strip
//   4px  gap
//   10px CLOCK OUT label
//   22px red name strip
//   6px  bottom padding
// ══════════════════════════════════════════════════════════════════════════════
#define Z1_Y    0
#define Z1_H   36   // Header
#define Z2_Y   36
#define Z2_H   20   // Status badges
#define Z3_Y   56
#define Z3_H   80   // 24H clock
// Sub-positions inside Z3, chosen so the font-7 clock digits (48px tall)
// never overlap the "24H LOCAL TIME" label above or the "HRS" label below:
//   Z3_Y+4 (60)              -- "24H LOCAL TIME" label, ~8px tall -> ends ~68
//   Z3_CLOCK_CY (96)         -- vertical CENTER of the clock digits (48px
//                                tall -> spans 72-120), leaves a 4px gap on
//                                both sides
//   Z3_HRS_Y (128)           -- "HRS" label center, ~8px tall -> spans
//                                124-132, i.e. a 4px gap below the digits
#define Z3_CLOCK_CY  (Z3_Y + 40)
#define Z3_HRS_Y     (Z3_Y + Z3_H - 8)
#define Z4_Y  136
#define Z4_H   24   // Date
#define Z5_Y  160
#define Z5_H    4   // Divider
#define Z6_Y  164
#define Z6_H   66   // Stat cards
#define Z7_Y  230
#define Z7_H    4   // Divider
#define Z8_Y  234
#define Z8_H   76   // Clock-In label+strip / Clock-Out label+strip
// Z8 ends at Y=310, leaving 10px margin to screen bottom (320)

// ══════════════════════════════════════════════════════════════════════════════
// API
// ══════════════════════════════════════════════════════════════════════════════
void dashboardInit();
void showLoadingAnimation(int progress, const char* message);
void drawStaticUI();
void updateClock(uint8_t h, uint8_t m, uint8_t s);
void updateDate(const String& dateStr);
void updateAttendanceStats(int checkIns, int checkOuts);
void updateLastScan(const String& name, const String& eventType, const String& timeStr);
void clearLastScan(const String& eventType);  // "check-in" | "check-out" | "both"
void updateStatusDots(bool wifiOk, bool sdOk, bool nfcOk);
void pulseStatus(bool state);
TFT_eSPI* dashboardGetTFT();

// ── Change notice (toast) ───────────────────────────────────────────────────
// Small 5-second banner shown over the status-dots row (Z2) whenever the
// attendance shown on screen changes from something OTHER than a live local
// NFC tap — e.g. a row deleted from the web portal, or a record removed by
// the background server-reconciliation pass. Call showChangeNotice() the
// moment such a change is applied, then call tickChangeNotice() once per
// loop() iteration; it auto-restores the normal status row after ~5s.
void showChangeNotice(const String& message);
void tickChangeNotice();