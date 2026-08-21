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
#include "sd_logger.h"
#include "sd_file_manager.h"

// ── SDMMC 1-bit pin definitions ───────────────────────────────────────────────
#define SD_MMC_CLK_PIN  39
#define SD_MMC_CMD_PIN  38
#define SD_MMC_D0_PIN   40

// ── Date provider callback ────────────────────────────────────────────────────
// Set this from main.cpp so SDDatabase uses the real NTP date for filenames.
// Signature: returns "YYYY-MM-DD" or "" if clock not yet synced.
typedef String (*DateProviderFn)();

class SDDatabase {
public:
    // Call once after NTP sync is available, e.g.:
    //   SDDatabase::setDateProvider([]() { return dateStr(); });
    static void setDateProvider(DateProviderFn fn);

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
    // Drops the cached in-memory check-in/check-out counters so the next
    // countTodayCheckIns()/countTodayCheckOuts() call re-scans the CSV from
    // scratch and reprimes the cache. Call this on midnight rollover (new
    // CSV file) and after any out-of-band edit to today's CSV that doesn't
    // go through logAttendance()/removeAttendanceRow() (e.g. a restored
    // backup file dropped onto the card).
    static void   resetTodayCountCache();

    static bool saveEmployeeProfile(const String& empUid, const EmployeeProfile& emp);
    static bool loadEmployeeProfile(const String& empUid, EmployeeProfile& out);
    static bool hasEmployeeProfile(const String& empUid);
    static bool   savePhoto(const String& empUid, const uint8_t* data, size_t length);
    static bool   hasPhoto(const String& empUid);
    static String photoPath(const String& empUid);

    static bool   hasCheckedInToday(const String& empUid);

    // Returns comma-separated list of clock_types logged today for empUid.
    // e.g. "morning_in,morning_out,afternoon_in"
    // Returns "" if no records exist for this employee today.
    static String loadAttendanceToday(const String& empUid);

    // Removes every row in TODAY's CSV matching (empUid, clockType).
    // Rewrites the whole file (read-filter-write) since SD CSVs are
    // append-only and there is no in-place line delete on FAT32.
    // Used by server-reconciliation: when a record is deleted server-side,
    // the local seed/mirror copy needs to be dropped too, or every future
    // sync pass will just keep skipping it as "already in CSV".
    // Returns true if the file is in the desired state afterward
    // (including the no-op case where nothing matched).
    static bool removeAttendanceRow(const String& empUid, const String& clockType);
    static bool   saveNfcMapping(const String& cardId, const String& empUid);
    static String loadUidForNfc(const String& cardId);
    // Fallback for short/truncated NDEF reads: scans /employees for exactly
    // ONE nfc_<prefix>*.json match. Returns "" (never guesses) on 0 or 2+
    // matches so an ambiguous or unknown prefix always fails safe.
    static String loadUidForNfcPrefix(const String& prefix);

    static uint64_t freeBytes();
    static void     printInfo();

    // ── Server-ID map ─────────────────────────────────────────────────────
    // Persists the server DB `id` returned after an attendance POST so that
    // subsequent edits can use PUT /{id} instead of POST (preventing duplicates).
    // Stored at /attendance/server_ids_YYYY-MM-DD.json
    // Key format: "empUid|clockType|HH:MM:SS"  (unique per record)
    static bool   saveServerIdMapping(const String& date, const String& empUid,
                                      const String& clockType, const String& timeStr,
                                      int serverId);
    static int    getServerIdForRecord(const String& date, const String& empUid,
                                       const String& clockType, const String& timeStr);
    // Returns entire map as a JSON string {"key": id, ...} for WiFiManager
    static String loadServerIdMapJson(const String& date);
    // Remove a single entry from the map (call after SD row is deleted)
    static bool   removeServerIdMapping(const String& date, const String& empUid,
                                        const String& clockType, const String& timeStr);

private:
    static bool          _ready;
    static DateProviderFn _dateProvider;
    // In-memory cache for countTodayCheckIns()/countTodayCheckOuts() — avoids
    // re-scanning the whole day's CSV on every call (was measured at 50-200ms
    // per scan, called from ~8 sites in main.cpp). -1 means "not primed yet
    // this session", which forces one real scan on first use; after that,
    // logAttendance()/removeAttendanceRow() keep these in sync incrementally
    // so no further full scans happen until resetTodayCountCache() is called
    // (midnight rollover) or the device reboots.
    static int    _cachedIns;
    static int    _cachedOuts;
    static bool   ensureDir(const char* path);
    static String todayFilename();
    static String csvEscape(const String& s);

    SDDatabase() = delete;
};