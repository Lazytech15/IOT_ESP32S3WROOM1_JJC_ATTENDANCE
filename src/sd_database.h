// ══════════════════════════════════════════════════════════════════════════════
// sd_database.h
// SD Card Database — NFC Scan Logs + Employee Profile Cache
//
// !! IMPORTANT: Uses SD_MMC (SDMMC 1-bit mode), NOT SPI SD.
//    Required for Freenove ESP32-S3 WROOM board.
//
//    Pins (SDMMC 1-bit):
//      CLK → GPIO 39
//      CMD → GPIO 38
//      D0  → GPIO 40
//
// Directory layout on SD card:
//   /attendance/    — daily CSV scan logs
//   /employees/     — JSON employee profiles (keyed by employee UID)
//   /photos/        — JPEG profile images   (keyed by employee UID)
// ══════════════════════════════════════════════════════════════════════════════

#pragma once
#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include <ArduinoJson.h>
#include "employee_profile_display.h"

// ── SDMMC 1-bit pin definitions ───────────────────────────────────────────────
#define SD_MMC_CLK_PIN  39
#define SD_MMC_CMD_PIN  38
#define SD_MMC_D0_PIN   40

class SDDatabase {
public:
    static bool begin();
    static bool isReady();

    static bool logAttendance(const String& timestamp,
                               const String& nfcUid,
                               const EmployeeProfile& emp,
                               const String& eventType,
                               const String& deviceId);

    static String readTodayCSV();
    static String readCSV(const String& dateOrPath);
    static String listAttendanceDates();
    static int    countTodayCheckIns();
    static int    countTodayCheckOuts();

    static bool saveEmployeeProfile(const String& empUid, const EmployeeProfile& emp);
    static bool loadEmployeeProfile(const String& empUid, EmployeeProfile& out);
    static bool hasEmployeeProfile(const String& empUid);

    static bool   savePhoto(const String& empUid, const uint8_t* data, size_t length);
    static bool   hasPhoto(const String& empUid);
    static String photoPath(const String& empUid);

    static bool   hasCheckedInToday(const String& empUid);
    static bool   saveNfcMapping(const String& cardId, const String& empUid);
    static String loadUidForNfc(const String& cardId);

    static uint64_t freeBytes();
    static void     printInfo();

private:
    static bool   _ready;
    static bool   ensureDir(const char* path);
    static String todayFilename();
    static int    countEventInCSV(const String& path, const String& eventType);
    static String csvEscape(const String& s);
    SDDatabase() = delete;
};