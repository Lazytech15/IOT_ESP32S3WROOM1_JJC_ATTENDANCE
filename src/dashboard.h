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