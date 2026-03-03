// ══════════════════════════════════════════════════════════════════════════════
// dashboard.h  — Portrait Edition (240×320)
// UI/UX: Tactical HUD / Military 24H Telemetry (Adjusted)
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "TFTDisplayManager.h"

#define SCREEN_W   240
#define SCREEN_H   320

// ══════════════════════════════════════════════════════════════════════════════
// Zone layout (Y, H) — portrait 240×320
// ══════════════════════════════════════════════════════════════════════════════
#define Z1_Y    0
#define Z1_H   36   // Telemetry Header (Expanded for JJC 2-line title)
#define Z2_Y   36
#define Z2_H   20   // Tactical Status Badges
#define Z3_Y   56
#define Z3_H   80   // MASSIVE 24H MILITARY CLOCK
#define Z4_Y  136
#define Z4_H   24   // ISO Date String
#define Z5_Y  160
#define Z5_H    4   // HUD Divider
#define Z6_Y  164
#define Z6_H   66   // Wireframe Stat Cards
#define Z7_Y  230
#define Z7_H    4   // HUD Divider
#define Z8_Y  234
#define Z8_H   60   // Terminal Event Log (Last Scan)
#define Z9_Y  294
#define Z9_H   26   // System Footer

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
void updateStatusDots(bool wifiOk, bool sdOk, bool nfcOk);
void pulseStatus(bool state);
TFT_eSPI* dashboardGetTFT();