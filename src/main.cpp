// ════════════════════════════════════════════════════════════════════════════
// main.cpp — ESP32-S3 WROOM-1  NFC Attendance  (Portrait 240×320)
//
// OPTIMIZATIONS v3:
//
//  1. FAST BOOT       — SD, NFC, WiFi, NTP start immediately. No blocking
//                       delays after each step. Total target boot < 4s.
//
//  2. FAST TAP        — NFC → SD write → profile display in < 200ms.
//                       Profile shown for exactly PROFILE_DISPLAY_MS (2s),
//                       then auto-returns to dashboard for next tap.
//
//  3. OFFLINE-FIRST   — Every scan is ALWAYS written to SD first (< 5ms).
//                       In-memory queue holds unsynced records; flushPending()
//                       drains them to server in the background.
//
//  4. PEAK-HOUR DEFER — During 11:55-12:05 (noon out) and 16:55-17:05
//                       (afternoon out) upload is DEFERRED to local-SD-only
//                       mode. flushPending() won't run until PEAK_DEFER_MIN
//                       (10 min) after the peak window. Outside peak, records
//                       sync to the server in ~500ms (near-instant).
//
//  5. MANUAL SYNC     — Employee profiles and photos are downloaded ONLY via
//                       the web portal "Sync" action. The system no longer
//                       auto-downloads employees at boot unless the SD cache
//                       is completely empty. Seeding (today's attendance from
//                       server) is also triggered manually or on boot.
//
//  6. MANUAL RESEED   — Portal action "Reseed Today" fetches fresh attendance
//                       from the server. Useful after deleting local data.
//                       The periodic auto-reseed is kept as a light background
//                       task but does NOT run during peak hours.
// ════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "TFTDisplayManager.h"
#include "dashboard.h"
#include "nfc_manager.h"
#include "WiFiConfig.h"
#include "WiFiManager.h"
#include "pin_assignments.h"
#include "employee_profile_display.h"
#include "attendance_http_service.h"
#include "sd_database.h"
#include "employee_sync.h"

// ─── Timing constants ────────────────────────────────────────────────────────
#define NFC_POLL_INTERVAL_MS      50    // PN532 poll cadence (tightened for speed with hardware SPI)
#define CLOCK_UPDATE_MS         1000
#define STATS_REFRESH_MS       30000
#define WIFI_RETRY_MS          20000

// ── PROFILE DISPLAY ───────────────────────────────────────────────────────────
// How long the employee photo/badge stays on screen after a scan.
// 2000ms = 2 seconds — just enough for the card-holder to see their name,
// then dashboard returns immediately for the next employee.
#define PROFILE_DISPLAY_MS      2000

// ── UPLOAD SCHEDULER ──────────────────────────────────────────────────────────
#define UPLOAD_FLUSH_MS          500    // background upload cadence (normal hours) — near-instant outside peak
#define UPLOAD_BATCH_SIZE          1    // one HTTP call per cycle → predictable latency
#define SOCKET_POLL_MS          8000
#define RESEED_INTERVAL_MS    300000    // auto-reseed every 5 min (skipped during peak)

// ── PEAK HOUR DEFERRAL ────────────────────────────────────────────────────────
// During rush windows the upload queue is NOT flushed to the server.
// All records still land on SD immediately. Uploads resume PEAK_DEFER_MIN
// minutes AFTER the peak window ends, giving the server time to breathe.
//
// Peak windows (device local time, 24h):
//   Noon out:      11:50 – 12:10
//   Afternoon out: 16:50 – 17:10
#define PEAK_DEFER_MIN          10      // minutes of post-peak upload hold (reduced from 20)

// ─── NFC scan-gate constants ─────────────────────────────────────────────────
#define MIN_CARD_ID_LEN          8
#define SCAN_COOLDOWN_MS       3500    // same-card lockout
#define CARD_CONFIRM_NEEDED      1

#define AP_SSID     "JJC_Attendance_Config"
#define AP_PASSWORD "ilovejjcenggworks"
#define SERVER_URL  "https://jjcenggworks.com"

String deviceId = "Attendance_Display_01";

// ─── Software clock ──────────────────────────────────────────────────────────
static uint8_t  clkH = 0, clkM = 0, clkS = 0;
static uint32_t clkEpoch = 0;

static void tickClock() {
    if (++clkS >= 60) { clkS = 0;
    if (++clkM >= 60) { clkM = 0;
    if (++clkH >= 24) { clkH = 0; if (clkEpoch) clkEpoch += 86400; }}}
    if (clkEpoch) clkEpoch++;
}

static int catchUpClock(unsigned long& lastClock) {
    unsigned long now = millis();
    unsigned long elapsed = now - lastClock;
    if (elapsed < 1000) return 0;
    int ticks = (int)(elapsed / 1000);
    if (ticks > 60) ticks = 60;
    lastClock += (unsigned long)ticks * 1000;
    for (int i = 0; i < ticks; i++) tickClock();
    return ticks;
}
static String clockStr() {
    char b[12]; snprintf(b, sizeof(b), "%02d:%02d:%02d", clkH, clkM, clkS);
    return b;
}
static String dateStr() {
    if (!clkEpoch) return "";
    time_t t = (time_t)clkEpoch;
    struct tm* tm = gmtime(&t);
    char b[12];
    snprintf(b, sizeof(b), "%04d-%02d-%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return b;
}
static String buildDateStr() {
    if (clkEpoch) {
        static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        time_t t = (time_t)clkEpoch;
        struct tm* tm = gmtime(&t);
        char b[32];
        snprintf(b, sizeof(b), "%s - %04d-%02d-%02d",
                 days[tm->tm_wday],
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        return String(b);
    }
    return "--- AWAITING SYNC ---";
}

// ── PEAK HOUR DETECTION ───────────────────────────────────────────────────────
// Returns true if clock is currently inside a peak upload-defer window OR
// within PEAK_DEFER_MIN minutes after one ended.
//
// Peak windows (device-local 24h):
//   11:55–12:05 = noon out rush    (centred on 12:00)
//   16:55–17:05 = afternoon out rush (centred on 17:00)
//
// Stores the minute at which the last peak window ended (peakEndMin) so that
// the post-peak hold is counted in wall-clock minutes, not milliseconds.
static uint16_t peakEndMin = 0xFFFF;  // 0xFFFF = no peak seen yet

static bool isPeakHour() {
    uint16_t nowMin = (uint16_t)clkH * 60 + clkM;

    // Define peak windows [startMin, endMin] inclusive
    struct { uint16_t s, e; } windows[] = {
        { 11*60+55, 12*60+ 5 },   // noon out    (centred on 12:00)
        { 16*60+55, 17*60+ 5 },   // afternoon out (centred on 17:00)
    };

    for (auto& w : windows) {
        if (nowMin >= w.s && nowMin <= w.e) {
            peakEndMin = w.e;     // update so post-peak hold works
            return true;
        }
    }

    // Post-peak deferral: hold uploads for PEAK_DEFER_MIN after window
    if (peakEndMin != 0xFFFF) {
        uint16_t holdUntil = peakEndMin + PEAK_DEFER_MIN;
        if (nowMin > peakEndMin && nowMin <= holdUntil) {
            return true;  // still in post-peak hold
        }
        if (nowMin > holdUntil) {
            peakEndMin = 0xFFFF;  // hold expired, clear flag
        }
    }

    return false;
}

// ─── Globals ─────────────────────────────────────────────────────────────────
WiFiConfig             wifiConfig(AP_SSID, AP_PASSWORD);
WiFiManager            wifiManager;
AttendanceHTTPService  attService(SERVER_URL);
EmployeeProfileDisplay* empDisplay = nullptr;

static bool initialSyncDone = false;
static unsigned long g_forceReseedAt = 0xFFFFFFFFUL;

// Flags settable from web portal actions (checked in loop)
volatile bool g_triggerEmployeeSync = false;   // download employees from server
volatile bool g_triggerPhotoSync    = false;   // download missing photos
volatile bool g_triggerReseedToday  = false;   // fetch today's attendance from server

// ─── State machine ────────────────────────────────────────────────────────────
enum SystemState : uint8_t {
    STATE_DASHBOARD, STATE_NFC_LOADING, STATE_NFC_PROFILE, STATE_NFC_ERROR
};
SystemState   currentState   = STATE_DASHBOARD;
unsigned long stateEnteredAt = 0;
String        lastNFCUid     = "";
EmployeeProfile lastEmployee;
String        lastClockType  = "morning_in";

static void          enterState(SystemState s) { currentState = s; stateEnteredAt = millis(); }
static unsigned long stateElapsed()            { return millis() - stateEnteredAt; }

// ─── NFC scan-gate state ─────────────────────────────────────────────────────
static unsigned long lastAcceptedScanAt = 0;
static String        lastAcceptedCardId = "";
static String        _lastRawCard       = "";
static uint8_t       _cardConfirmCt     = 0;

static bool cardConfirmed(const String& cardId) {
    if (cardId == _lastRawCard) {
        if (_cardConfirmCt < 255) _cardConfirmCt++;
    } else {
        _lastRawCard   = cardId;
        _cardConfirmCt = 1;
    }
    return (_cardConfirmCt >= CARD_CONFIRM_NEEDED);
}

// ════════════════════════════════════════════════════════════════════════════
// CLOCK TYPE RESOLUTION
// ════════════════════════════════════════════════════════════════════════════
static String resolveClockType(const String& empUid) {
    static const char* SESSIONS[] = {
        "morning", "afternoon", "evening", nullptr
    };

#if defined(SDDB_HAS_LOAD_TODAY_CLOCK_TYPES)
    String existing = SDDatabase::loadTodayClockTypes(empUid);
    bool hasIn[3]  = {false, false, false};
    bool hasOut[3] = {false, false, false};
    for (int si = 0; SESSIONS[si]; si++) {
        String inKey  = String(SESSIONS[si]) + "_in";
        String outKey = String(SESSIONS[si]) + "_out";
        if (existing.indexOf(inKey)  >= 0) hasIn[si]  = true;
        if (existing.indexOf(outKey) >= 0) hasOut[si] = true;
    }
    for (int si = 0; SESSIONS[si]; si++) {
        if (hasIn[si] && !hasOut[si]) return String(SESSIONS[si]) + "_out";
    }
    for (int si = 0; SESSIONS[si]; si++) {
        if (!hasIn[si]) return String(SESSIONS[si]) + "_in";
    }
    return "morning_in";

#else
    if (!SDDatabase::isReady()) return "morning_in";

    String todayLog = SDDatabase::loadAttendanceToday(empUid);

    if (todayLog.length() == 0) {
        bool anyRecord = SDDatabase::hasCheckedInToday(empUid);
        return anyRecord ? "morning_out" : "morning_in";
    }

    bool hasIn[3]  = {false, false, false};
    bool hasOut[3] = {false, false, false};
    for (int si = 0; SESSIONS[si]; si++) {
        String inKey  = String(SESSIONS[si]) + "_in";
        String outKey = String(SESSIONS[si]) + "_out";
        if (todayLog.indexOf(inKey)  >= 0) hasIn[si]  = true;
        if (todayLog.indexOf(outKey) >= 0) hasOut[si] = true;
    }

    for (int si = 0; SESSIONS[si]; si++) {
        if (hasIn[si] && !hasOut[si]) return String(SESSIONS[si]) + "_out";
    }
    for (int si = 0; SESSIONS[si]; si++) {
        if (!hasIn[si]) return String(SESSIONS[si]) + "_in";
    }
    return "morning_in";
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// PENDING ATTENDANCE QUEUE  (in-memory + SD-backed)
//
// Every NFC scan writes to SD immediately, then adds to this queue.
// flushPending() drains the queue to the server in the background.
// During peak hours the flush is skipped — records stay on SD.
// ════════════════════════════════════════════════════════════════════════════
struct PendingRecord {
    String empUid, nfcUid, clockType, timestamp, date;
};
static PendingRecord pendingQueue[32];
static int           pendingCount = 0;

static void enqueuePending(const String& empUid, const String& nfcUid,
                           const String& clockType, const String& ts,
                           const String& dt) {
    if (pendingCount < 32) {
        pendingQueue[pendingCount++] = {empUid, nfcUid, clockType, ts, dt};
        Serial.printf("[Queue] +1 pending (%d/32): %s %s\n",
                      pendingCount, empUid.c_str(), clockType.c_str());
    } else {
        Serial.println("[Queue] FULL — dropping oldest");
        for (int i = 0; i < 31; i++) pendingQueue[i] = pendingQueue[i+1];
        pendingQueue[31] = {empUid, nfcUid, clockType, ts, dt};
    }
}

// ── flushPending ──────────────────────────────────────────────────────────────
// Sends up to UPLOAD_BATCH_SIZE queued records to the server.
// Skipped automatically during peak hours — see isPeakHour().
static void flushPending() {
    if (!wifiConfig.isConnected() || pendingCount == 0) return;

    // ── PEAK HOUR GATE ────────────────────────────────────────────────────
    if (isPeakHour()) {
        Serial.printf("[Flush] PEAK HOUR — deferring upload (%d queued, will resume in ~%dmin)\n",
                      pendingCount, PEAK_DEFER_MIN);
        return;
    }

    int toSend = min(pendingCount, UPLOAD_BATCH_SIZE);
    Serial.printf("[Flush] Uploading %d/%d queued record(s)...\n", toSend, pendingCount);

    int remaining = 0;
    PendingRecord keep[32];

    for (int i = 0; i < pendingCount; i++) {
        if (i < toSend) {
            bool ok = attService.recordAttendance(
                pendingQueue[i].empUid, pendingQueue[i].nfcUid,
                deviceId, pendingQueue[i].clockType,
                pendingQueue[i].timestamp, pendingQueue[i].date);

            if (!ok) {
                keep[remaining++] = pendingQueue[i];
                Serial.printf("[Flush] RETRY later: %s %s\n",
                              pendingQueue[i].empUid.c_str(),
                              pendingQueue[i].clockType.c_str());
            } else {
                Serial.printf("[Flush] OK: %s %s\n",
                              pendingQueue[i].empUid.c_str(),
                              pendingQueue[i].clockType.c_str());
            }
            yield();
        } else {
            keep[remaining++] = pendingQueue[i];
        }
    }

    for (int j = 0; j < remaining; j++) pendingQueue[j] = keep[j];
    pendingCount = remaining;
    Serial.printf("[Flush] Done. %d remain in queue.\n", pendingCount);
}

// ════════════════════════════════════════════════════════════════════════════
// SOCKET POLLER — employee-only events
// ════════════════════════════════════════════════════════════════════════════
static void pollSocketEvents() {
    if (!wifiConfig.isConnected()) return;
    int empChanges = EmployeeSync::pollChanges(attService, String(SERVER_URL));
    if (empChanges > 0) {
        Serial.printf("[Socket] %d employee change(s) applied to SD cache\n", empChanges);
        if (currentState == STATE_DASHBOARD) {
            updateStatusDots(wifiConfig.isConnected(), SDDatabase::isReady(), true);
        }
    }
}

// ─── NTP Time Sync ────────────────────────────────────────────────────────────
void syncNTPTime() {
    if (!wifiConfig.isConnected()) return;
    Serial.println("[Time] Syncing NTP...");
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");

    struct tm timeinfo;
    int attempts = 0;
    while (!getLocalTime(&timeinfo, 1000) && attempts < 10) {
        attempts++;
        Serial.printf("[Time] Waiting for NTP... attempt %d\n", attempts);
        yield();
    }

    if (getLocalTime(&timeinfo, 0)) {
        clkH     = timeinfo.tm_hour;
        clkM     = timeinfo.tm_min;
        clkS     = timeinfo.tm_sec;
        clkEpoch = (uint32_t)mktime(&timeinfo);
        SDDatabase::setDateProvider([]() -> String { return dateStr(); });
        Serial.printf("[Time] OK: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
                      clkH, clkM, clkS);
    } else {
        Serial.println("[Time] NTP failed after 10s");
    }
}

// ─── Photo cache helper ───────────────────────────────────────────────────────
// NOTE: Photo downloads only happen here as a fallback when a card is scanned
// and no photo is on SD yet. The primary way to populate photos is via the
// web portal "Sync Photos" action (manual, no auto-download at boot).
static String getPhotoPath(const EmployeeProfile& emp) {
    if (!SDDatabase::isReady() || emp.uid.length() == 0) return "";
    if (SDDatabase::hasPhoto(emp.uid)) {
        String cand = SDDatabase::photoPath(emp.uid);
        if (SD_MMC.exists(cand)) {
            File f = SD_MMC.open(cand, FILE_READ);
            if (f && f.size() >= 100) { f.close(); return cand; }
        }
    }
    // Only attempt download if WiFi is available — do NOT block NFC path here
    // during peak hours; just return "" (profile shows without photo).
    if (!wifiConfig.isConnected() || isPeakHour()) return "";

    Serial.println("[Photo] Downloading uid=" + emp.uid);
    uint8_t* buf = nullptr; int len = 0;
    bool ok = attService.downloadProfileImage(emp.uid, &buf, &len, emp.profilePicture);
    if (!ok || !buf) { Serial.println("[Photo] Download failed"); return ""; }
    bool saved = SDDatabase::savePhoto(emp.uid, buf, (size_t)len);
    free(buf);
    if (saved) {
        Serial.println("[Photo] Cached: " + SDDatabase::photoPath(emp.uid));
        return SDDatabase::photoPath(emp.uid);
    }
    return "";
}

// ════════════════════════════════════════════════════════════════════════════
// seedTodayAttendanceFromServer  (manual + boot)
//
// Pulls today's records from the server into the local SD CSV so that
// resolveClockType() is accurate after reboots or web-portal entries.
//
// Called:
//   • Once at boot after NTP sync (triggerInitialSync)
//   • On demand via web portal "Reseed Today" action
//   • Periodically by loop() every RESEED_INTERVAL_MS — skipped during peak
// ════════════════════════════════════════════════════════════════════════════
static void seedTodayAttendanceFromServer() {
    if (!wifiConfig.isConnected()) return;
    if (!SDDatabase::isReady()) return;

    String today = dateStr();
    if (today.length() < 10) {
        Serial.println("[Seed] Skipping — clock not yet synced");
        return;
    }

    Serial.println("[Seed] === Seeding today's attendance: " + today + " ===");
    int seeded = 0;

    // PASS 1: Raw live endpoint (always fresh)
    int rawResult = attService.seedCsvFromRawAttendance(today);
    if (rawResult > 0) {
        seeded += rawResult;
        Serial.printf("[Seed] PASS 1: %d new row(s) seeded\n", rawResult);
    } else if (rawResult == 0) {
        Serial.println("[Seed] PASS 1: 0 new rows (up-to-date or no data)");
    } else {
        Serial.println("[Seed] PASS 1: network error");
    }

    // PASS 2: ESP32-sync snapshot (keeps JSON file current for portal fallback)
    int syncResult = attService.fetchTodayAttendanceEsp32(today);
    if (syncResult > 0) {
        seeded += syncResult;
        Serial.printf("[Seed] PASS 2: %d additional row(s) from esp32-sync\n", syncResult);
    }

    updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                          max(0, SDDatabase::countTodayCheckOuts()));
    Serial.printf("[Seed] === Seed complete: %d total new row(s) ===\n", seeded);
}

// ─── triggerInitialSync ───────────────────────────────────────────────────────
// Called once when WiFi first connects. Does NOT force-download employees
// if the SD cache already exists — manual sync via portal is used instead.
static void triggerInitialSync() {
    if (!wifiConfig.isConnected()) {
        Serial.println("[Sync] SKIP: no WiFi — SD-only mode");
        return;
    }
    if (initialSyncDone) return;
    initialSyncDone = true;

    Serial.println("[Sync] WiFi available — checking employee cache (SD-first)");

    // Only run full employee sync if cache is completely empty.
    // Otherwise, let the user trigger it from the portal.
    bool hasCache = SDDatabase::isReady() &&
                    SD_MMC.exists("/employees") &&
                    SD_MMC.exists("/employees/sync_meta.json");
    if (!hasCache) {
        Serial.println("[Sync] SD cache empty — running first-time employee sync");
        EmployeeSync::fullSyncIfNeeded(attService, true);
    } else {
        Serial.println("[Sync] SD cache present — skipping auto employee sync");
    }

    // Seed today's attendance from server
    seedTodayAttendanceFromServer();

    // Broadcast stats to portal
    {
        int ins  = max(0, SDDatabase::countTodayCheckIns());
        int outs = max(0, SDDatabase::countTodayCheckOuts());
        uint64_t freeMB = SDDatabase::freeBytes() / 1048576;
        bool wOk = wifiConfig.isConnected();
        String statsJson = "{\"ins\":" + String(ins)
                         + ",\"outs\":" + String(outs)
                         + ",\"free_mb\":" + String((int)freeMB)
                         + ",\"wifi\":" + (wOk ? "true" : "false") + "}";
        wifiManager.broadcastEvent("stats", statsJson);
    }

    if (pendingCount > 0 && !isPeakHour()) flushPending();

    drawStaticUI();
    updateStatusDots(true, SDDatabase::isReady(), true);
    updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                          max(0, SDDatabase::countTodayCheckOuts()));
    Serial.println("[Sync] Complete");
}

// ════════════════════════════════════════════════════════════════════════════
// handleNFCDetected  — FAST-TAP (< 200ms to profile display)
//
// 1. Resolve employee UID → SD mapping
// 2. Load profile from SD  (offline-capable)
// 3. Server fallback only if SD miss AND WiFi up AND not peak hour
// 4. Check photo on SD (no download during peak)
// 5. Determine clock type from today's SD records
// 6. Show profile card immediately
// 7. Log to SD  (< 5ms — guaranteed local record)
// 8. Enqueue for background upload (returns instantly)
// 9. Broadcast SSE to portal
// ════════════════════════════════════════════════════════════════════════════
static void handleNFCDetected(const String& cardIdentifier) {
    Serial.println("\n================================================");
    Serial.println("[NFC] Card: " + cardIdentifier);
    Serial.printf("[NFC] Heap=%u PSRAM=%u SD=%s WiFi=%s Q=%d Peak=%s\n",
        ESP.getFreeHeap(), ESP.getFreePsram(),
        SDDatabase::isReady() ? "OK" : "NO",
        wifiConfig.isConnected() ? "OK" : "NO",
        pendingCount,
        isPeakHour() ? "YES" : "NO");

    lastNFCUid = cardIdentifier;
    enterState(STATE_NFC_LOADING);
    empDisplay->showLoading();

    EmployeeProfile emp;
    bool fromCache = false;

    // ── STEP 1: Resolve UID ───────────────────────────────────────────────
    String empUid = SDDatabase::loadUidForNfc(cardIdentifier);
    if (empUid.length() == 0 && SDDatabase::hasEmployeeProfile(cardIdentifier))
        empUid = cardIdentifier;

    // ── STEP 2: Load from SD ──────────────────────────────────────────────
    if (empUid.length() > 0 && SDDatabase::hasEmployeeProfile(empUid)) {
        if (SDDatabase::loadEmployeeProfile(empUid, emp)) {
            fromCache = true;
            Serial.printf("[STEP-2] SD hit: %s\n", emp.fullName.c_str());
        }
    }

    // ── STEP 3: Server fallback (only if SD miss, WiFi up, not peak) ──────
    if (!fromCache) {
        if (!wifiConfig.isConnected()) {
            empDisplay->showError("Unknown Card\n(Offline)");
            enterState(STATE_NFC_ERROR);
            return;
        }
        // During peak, skip slow server auth — unknown cards get a brief error
        if (isPeakHour()) {
            empDisplay->showError("Unknown Card\n(Peak Mode)");
            enterState(STATE_NFC_ERROR);
            return;
        }

        Serial.println("[STEP-3] Fetching from server...");
        bool granted = attService.authenticateNFC(cardIdentifier, deviceId, emp);
        if (!granted) {
            empDisplay->showError(emp.hasData ? "Access Denied" : "Card Not Registered");
            enterState(STATE_NFC_ERROR);
            return;
        }

        if (SDDatabase::isReady() && emp.uid.length() > 0) {
            SDDatabase::saveEmployeeProfile(emp.uid, emp);
            SDDatabase::saveNfcMapping(cardIdentifier, emp.uid);
        }
    }

    // ── STEP 3b: Inactive check ───────────────────────────────────────────
    if (emp.hasData && emp.status.length() > 0 && emp.status != "Active") {
        Serial.printf("[NFC] Inactive employee '%s' — blocking\n", emp.fullName.c_str());
        empDisplay->showInactive(emp.fullName);
        enterState(STATE_NFC_ERROR);
        return;
    }

    // ── STEP 4: Photo path (SD only; no download during peak) ────────────
    String photoPath = getPhotoPath(emp);
    Serial.printf("[STEP-4] photo='%s'\n", photoPath.c_str());

    // ── STEP 5: Clock type ────────────────────────────────────────────────
    String clockType = resolveClockType(emp.uid);
    emp.clockType = clockType;
    lastClockType = clockType;
    lastEmployee  = emp;
    Serial.printf("[STEP-5] clockType=%s\n", clockType.c_str());

    // ── STEP 6: Show profile card ─────────────────────────────────────────
    empDisplay->showEmployeeProfile(emp, photoPath);

    // ── STEP 7: Log to SD immediately ────────────────────────────────────
    String ts        = clockStr();
    String todayDate = dateStr();

    if (SDDatabase::isReady()) {
        SDDatabase::logAttendance(ts, cardIdentifier, emp, clockType, deviceId);
        Serial.println("[STEP-7] SD logged");
    }

    // ── STEP 8: Enqueue (upload handled later by flushPending) ───────────
    enqueuePending(emp.uid, cardIdentifier, clockType, ts, todayDate);
    Serial.printf("[STEP-8] Enqueued. Queue=%d\n", pendingCount);

    // ── STEP 9: Broadcast SSE ─────────────────────────────────────────────
    {
        String dispType = clockType;
        dispType.replace("_", " ");
        char tsShort[6]; snprintf(tsShort, sizeof(tsShort), "%02d:%02d", clkH, clkM);
        String scanJson = "{\"name\":\"" + emp.fullName
                        + "\",\"type\":\"" + dispType
                        + "\",\"time\":\"" + String(tsShort) + "\"}";
        wifiManager.broadcastEvent("scan", scanJson);

        int ins  = max(0, SDDatabase::countTodayCheckIns());
        int outs = max(0, SDDatabase::countTodayCheckOuts());
        uint64_t freeMB = SDDatabase::freeBytes() / 1048576;
        String statsJson = "{\"ins\":" + String(ins)
                         + ",\"outs\":" + String(outs)
                         + ",\"free_mb\":" + String((int)freeMB)
                         + ",\"wifi\":true}";
        wifiManager.broadcastEvent("stats", statsJson);
    }

    empDisplay->showSuccess(emp.fullName);
    lastAcceptedScanAt = millis();
    lastAcceptedCardId = cardIdentifier;

    Serial.println("[NFC] Done → STATE_NFC_PROFILE (2s timer)");
    enterState(STATE_NFC_PROFILE);
}

// ─── setup ───────────────────────────────────────────────────────────────────
// Fast boot: no long delays between steps. All steps are sequential but each
// one is as tight as possible. Target: ready for NFC taps within 4 seconds.
void setup() {
    Serial.begin(115200);
    delay(100);   // minimal: just enough for USB CDC to settle

    SDLogger::beginSerial();
    SDLogger::flushEarlyBuffer();
    SDLogger::installPanicHandler();

    Serial.println("\n========================================");
    Serial.println("[Boot] JJC NFC Attendance System v3");
    Serial.printf("[Boot] %s  cores=%d  %dMHz\n",
                  ESP.getChipModel(), ESP.getChipCores(), getCpuFrequencyMhz());
    Serial.printf("[Boot] Heap=%u  PSRAM=%u\n", ESP.getFreeHeap(), ESP.getPsramSize());
    if (!psramFound())
        Serial.println("[Boot] WARNING: PSRAM not detected!");

    // ── 1. TFT ────────────────────────────────────────────────────────────
    if (!TFTDisplayManager::init(0)) {
        while (true) delay(2000);
    }
    dashboardInit();
    showLoadingAnimation(0, "Booting...");
    empDisplay = new EmployeeProfileDisplay();

    // ── 2. SD Card ────────────────────────────────────────────────────────
    showLoadingAnimation(20, "SD...");
    if (SDDatabase::begin()) {
        Serial.println("[Boot] SD OK");
    } else {
        showLoadingAnimation(20, "SD FAILED");
        Serial.println("[Boot] SD FAILED");
    }

    // ── 3. NFC ────────────────────────────────────────────────────────────
    showLoadingAnimation(35, "NFC...");
    if (!nfcInit()) {
        showLoadingAnimation(100, "NFC FAILED!");
        delay(500);
        while (true) delay(2000);
    }
    Serial.println("[Boot] NFC OK");

    // ── 4. WiFi (non-blocking connect attempt) ────────────────────────────
    showLoadingAnimation(50, "WiFi...");
    wifiConfig.begin();
    if (wifiConfig.isConnected()) {
        showLoadingAnimation(65, ("WiFi: " + wifiConfig.getSSID()).c_str());
        syncNTPTime();
    } else {
        showLoadingAnimation(65, "WiFi: AP mode");
        Serial.println("[Boot] WiFi not connected — SD-only");
    }

    // ── 5. Web portal ─────────────────────────────────────────────────────
    showLoadingAnimation(80, "Portal...");
    wifiManager.init(deviceId, &wifiConfig, SERVER_URL, &attService);

    // ── 6. Ready — draw dashboard immediately, THEN sync in background ────
    showLoadingAnimation(100, "READY!");
    delay(200);   // brief flash of READY before dashboard

    TFT_eSPI* t = dashboardGetTFT();
    t->fillScreen(TFTColors::BLACK);
    drawStaticUI();
    updateStatusDots(wifiConfig.isConnected(), SDDatabase::isReady(), true);
    updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                          max(0, SDDatabase::countTodayCheckOuts()));
    enterState(STATE_DASHBOARD);

    // Initial sync runs AFTER dashboard is live — NFC already accepts taps
    if (wifiConfig.isConnected()) {
        triggerInitialSync();
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
    } else {
        Serial.println("[Ready] SD-only — scanning active");
    }

    Serial.println("[Ready] NFC scanning active");
    Serial.printf("[Ready] Portal: http://%s:8080\n",
                  wifiConfig.isConnected()
                  ? wifiConfig.getIPAddress().c_str()
                  : WiFi.softAPIP().toString().c_str());
}

// ─── loop ────────────────────────────────────────────────────────────────────
void loop() {
    static unsigned long lastClock       = 0;
    static unsigned long lastNFCPoll     = 0;
    static unsigned long lastStats       = 0;
    static unsigned long lastWifiRetry   = 0;
    static unsigned long lastUploadFlush = 0;
    static unsigned long lastSocketPoll  = 0;
    static unsigned long lastReSeed      = 0;
    static bool          wasConnected    = false;
    static uint8_t       tick            = 0;
    unsigned long now = millis();

    wifiConfig.handleClient();
    wifiManager.handleClient();

    // ── WiFi connect / disconnect events ─────────────────────────────────
    bool isConnected = wifiConfig.isConnected();
    if (isConnected && !wasConnected) {
        wasConnected = true;
        Serial.println("[WiFi] Connected");
        syncNTPTime();
        triggerInitialSync();
        updateStatusDots(true, SDDatabase::isReady(), true);
        drawStaticUI();
    }
    if (!isConnected && wasConnected) {
        wasConnected = false;
        Serial.println("[WiFi] Lost — SD-only mode");
        updateStatusDots(false, SDDatabase::isReady(), true);
    }

    // ── WiFi reconnect retry ──────────────────────────────────────────────
    if (!isConnected && (now - lastWifiRetry >= WIFI_RETRY_MS)) {
        lastWifiRetry = now;
        WiFi.reconnect();
    }

    // ── Manual action triggers from web portal ────────────────────────────
    if (g_triggerEmployeeSync) {
        g_triggerEmployeeSync = false;
        if (isConnected && currentState == STATE_DASHBOARD) {
            Serial.println("[Action] Manual employee sync triggered");
            EmployeeSync::fullSyncIfNeeded(attService, true);
            drawStaticUI();
            updateStatusDots(isConnected, SDDatabase::isReady(), true);
        }
    }
    if (g_triggerReseedToday) {
        g_triggerReseedToday = false;
        if (isConnected && currentState == STATE_DASHBOARD) {
            Serial.println("[Action] Manual reseed triggered");
            seedTodayAttendanceFromServer();
            updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                                  max(0, SDDatabase::countTodayCheckOuts()));
        }
    }
    if (g_triggerPhotoSync) {
        g_triggerPhotoSync = false;
        if (isConnected && currentState == STATE_DASHBOARD) {
            Serial.println("[Action] Manual photo sync triggered");
            // Walk /employees/*.json and download any missing photo
            File root = SD_MMC.open("/employees");
            if (root) {
                File f = root.openNextFile();
                while (f) {
                    String name = String(f.name());
                    if (name.endsWith(".json") && !name.endsWith("sync_meta.json")
                        && !name.endsWith("photo_results.csv")) {
                        // Extract UID from filename
                        String uid = name;
                        int sl = uid.lastIndexOf('/');
                        if (sl >= 0) uid = uid.substring(sl + 1);
                        uid.replace(".json", "");
                        if (uid.length() > 0 && !SDDatabase::hasPhoto(uid)) {
                            Serial.println("[PhotoSync] Downloading: " + uid);
                            uint8_t* buf = nullptr; int len = 0;
                            if (attService.downloadProfileImage(uid, &buf, &len, "")) {
                                SDDatabase::savePhoto(uid, buf, (size_t)len);
                                free(buf);
                                Serial.println("[PhotoSync] Saved: " + uid);
                            }
                            yield();
                        }
                    }
                    f.close();
                    f = root.openNextFile();
                }
                root.close();
            }
            Serial.println("[PhotoSync] Done");
        }
    }

    // ── Socket event poller (employee profile updates) ────────────────────
    if (isConnected && currentState == STATE_DASHBOARD &&
        !isPeakHour() &&
        (now - lastSocketPoll >= SOCKET_POLL_MS)) {
        lastSocketPoll = now;
        if (ESP.getFreeHeap() > 40000) {
            pollSocketEvents();
        }
    }

    // ── Periodic SD re-seed from server (skipped during peak) ─────────────
    {
        bool timerFired = (now - lastReSeed >= RESEED_INTERVAL_MS);
        bool forceFired = (now >= g_forceReseedAt);
        if (isConnected && currentState == STATE_DASHBOARD &&
            SDDatabase::isReady() && dateStr().length() >= 10 &&
            !isPeakHour() &&
            (timerFired || forceFired)) {

            lastReSeed      = now;
            g_forceReseedAt = 0xFFFFFFFFUL;

            static bool useEsp32Sync = true;
            int rs = 0;
            if (useEsp32Sync) {
                rs = attService.fetchTodayAttendanceEsp32(dateStr());
                Serial.printf("[ReSeed] esp32-sync: %d row(s)\n", rs);
            } else {
                rs = attService.fetchTodayAttendance(dateStr());
                Serial.printf("[ReSeed] raw: %d row(s)\n", rs);
            }
            useEsp32Sync = !useEsp32Sync;

            if (rs > 0) {
                int ins  = max(0, SDDatabase::countTodayCheckIns());
                int outs = max(0, SDDatabase::countTodayCheckOuts());
                updateAttendanceStats(ins, outs);
                uint64_t freeMB = SDDatabase::freeBytes() / 1048576;
                String statsJson = "{\"ins\":" + String(ins)
                                 + ",\"outs\":" + String(outs)
                                 + ",\"free_mb\":" + String((int)freeMB)
                                 + ",\"wifi\":true}";
                wifiManager.broadcastEvent("stats", statsJson);
            }
        }
    }

    // ── Background upload scheduler ───────────────────────────────────────
    // Runs only when: WiFi up + records queued + on dashboard + not peak hour
    if (isConnected && pendingCount > 0 &&
        currentState == STATE_DASHBOARD &&
        (now - lastUploadFlush >= UPLOAD_FLUSH_MS)) {
        lastUploadFlush = now;
        flushPending();   // isPeakHour() check is inside flushPending()

        if (currentState == STATE_DASHBOARD) {
            int ins  = max(0, SDDatabase::countTodayCheckIns());
            int outs = max(0, SDDatabase::countTodayCheckOuts());
            updateAttendanceStats(ins, outs);
            uint64_t freeMB = SDDatabase::freeBytes() / 1048576;
            String statsJson = "{\"ins\":" + String(ins)
                             + ",\"outs\":" + String(outs)
                             + ",\"free_mb\":" + String((int)freeMB)
                             + ",\"wifi\":true}";
            wifiManager.broadcastEvent("stats", statsJson);
        }
    }

    // ── State machine ─────────────────────────────────────────────────────
    switch (currentState) {

        case STATE_NFC_PROFILE:
            // Wait exactly PROFILE_DISPLAY_MS (2s) then snap back to dashboard
            if (stateElapsed() >= PROFILE_DISPLAY_MS) {
                enterState(STATE_DASHBOARD);
                drawStaticUI();
                updateStatusDots(isConnected, SDDatabase::isReady(), true);
                updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                                      max(0, SDDatabase::countTodayCheckOuts()));
                if (lastEmployee.hasData) {
                    char timeShort[6];
                    snprintf(timeShort, sizeof(timeShort), "%02d:%02d", clkH, clkM);
                    bool wasIn = (lastClockType.endsWith("_in"));
                    updateLastScan(lastEmployee.fullName,
                                   wasIn ? "check-in" : "check-out",
                                   String(timeShort));
                }
            }
            break;

        case STATE_NFC_ERROR:
            if (stateElapsed() >= PROFILE_DISPLAY_MS) {
                enterState(STATE_DASHBOARD);
                drawStaticUI();
                updateStatusDots(isConnected, SDDatabase::isReady(), true);
                updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                                      max(0, SDDatabase::countTodayCheckOuts()));
            }
            break;

        default: break;
    }

    // ── Clock tick ────────────────────────────────────────────────────────
    if (currentState == STATE_DASHBOARD) {
        int ticked = catchUpClock(lastClock);
        if (ticked > 0) {
            tick += (uint8_t)ticked;
            pulseStatus(tick % 2);
            updateClock(clkH, clkM, clkS);
            if (tick % 60   == 0) updateDate(buildDateStr());
            if (tick % 3600 == 0 && isConnected) syncNTPTime();

            if (clkH == 0 && clkM == 0 && clkS == 0) {
                Serial.println("[Midnight] New day — resetting stats");
                peakEndMin = 0xFFFF;  // reset peak tracking at midnight
                drawStaticUI();
                updateStatusDots(isConnected, SDDatabase::isReady(), true);
                updateAttendanceStats(0, 0);
                updateDate(buildDateStr());
                SDDatabase::setDateProvider([]() -> String { return dateStr(); });
            }
        }
    }

    // ── Stats refresh ─────────────────────────────────────────────────────
    if (currentState == STATE_DASHBOARD && (now - lastStats >= STATS_REFRESH_MS)) {
        lastStats = now;
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
    }

    // ── NFC poll ──────────────────────────────────────────────────────────
    if (currentState != STATE_NFC_LOADING &&
        (now - lastNFCPoll >= NFC_POLL_INTERVAL_MS)) {
        lastNFCPoll = now;

        uint8_t uid[7] = {0}; uint8_t uidLen = 0;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) {
            nfcProcessCard(uid, uidLen);

            String cardId;
            if (nfcData.length() >= MIN_CARD_ID_LEN) {
                cardId = nfcData;
            } else if (nfcUID.length() >= MIN_CARD_ID_LEN) {
                cardId = nfcUID;
            } else {
                goto nfc_poll_end;
            }

            if (!cardConfirmed(cardId)) {
                // still counting confirmations

            } else if (cardId == lastAcceptedCardId &&
                       (now - lastAcceptedScanAt) < SCAN_COOLDOWN_MS) {
                // same card on reader — cooldown

            } else if (currentState != STATE_DASHBOARD) {
                // waiting for profile display to finish

            } else {
                _lastRawCard   = cardId;
                _cardConfirmCt = CARD_CONFIRM_NEEDED;
                handleNFCDetected(cardId);
            }

        } else {
            if (_cardConfirmCt > 0) { _lastRawCard = ""; _cardConfirmCt = 0; }
        }
        nfc_poll_end:;
    }
}
