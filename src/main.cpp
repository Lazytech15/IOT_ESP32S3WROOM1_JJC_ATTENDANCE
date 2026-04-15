// ════════════════════════════════════════════════════════════════════════════
// main.cpp — ESP32-S3 WROOM-1  NFC Attendance  (Portrait 240×320)
//
// ARCHITECTURE:
//   • SD-FIRST: Boots and operates 100% offline using only SD card data.
//               Internet is optional — used only to sync new employees and
//               push pending attendance records.
//   • CLOCK TYPE: Determines morning_in/morning_out/afternoon_in/afternoon_out
//                 /evening_in/evening_out from existing SD records.
//                 evening = afternoon overtime. overtime_in/out NOT used.
//                 SD records — matching the server attendance table exactly.
//   • SOCKET POLLER: Polls /api/socket?action=poll every SOCKET_POLL_MS ms for
//                    real-time attendance_created / attendance_updated events.
//                    On event: updates display stats and Z8 last-scan strip.
//   • SCAN GATES: noise filter, ghost-card debounce, cooldown.
//
// ── FAST-TAP UPLOAD SCHEDULER (v2) ──────────────────────────────────────────
//   Problem: attService.recordAttendance() was called INLINE during handleNFCDetected()
//            which blocked the NFC poller for ~1-3 seconds per scan (HTTP round-trip).
//            With queued employees this compounds into a 10-30s queue.
//
//   Solution: Every scan is ALWAYS logged to SD immediately (< 5ms), then
//             ENQUEUED into pendingQueue. A separate scheduled uploader
//             (UPLOAD_FLUSH_MS cadence) drains the queue in the background,
//             ONLY when the system is on the dashboard (not during a scan).
//             This means NFC taps are accepted at full speed (~150ms each)
//             regardless of server response time or connectivity.
//
//   Key change: handleNFCDetected() NEVER calls attService.recordAttendance().
//               All server POSTs happen via flushPending() only.
// ════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SD_MMC.h>
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
#define NFC_POLL_INTERVAL_MS     150    // PN532 poll cadence
#define CLOCK_UPDATE_MS         1000
#define STATS_REFRESH_MS       30000
#define WIFI_RETRY_MS          15000

// ── UPLOAD SCHEDULER TIMING ──────────────────────────────────────────────────
// UPLOAD_FLUSH_MS: How often the background uploader runs (in ms).
//   • 3000ms (3s) means the server gets records within ~3s of any tap.
//   • Set lower (e.g. 1500) for near-real-time push, higher (e.g. 5000)
//     to reduce server load. Do NOT set below 1000 — the HTTPClient
//     needs time to complete one round-trip before the next starts.
//   • The uploader only runs when WiFi is up AND the system is on
//     the dashboard, so it never interferes with active NFC display.
#define UPLOAD_FLUSH_MS         3000

// UPLOAD_BATCH_SIZE: Max records to POST per flush cycle.
//   Keeping this at 1 means one HTTP call per cycle — predictable latency.
//   Set to 3-5 if you want to catch up faster after an outage, but watch
//   for WDT timeouts on slow connections.
#define UPLOAD_BATCH_SIZE          3

#define PENDING_FLUSH_MS       60000   // legacy retry for any stragglers
#define SOCKET_POLL_MS          8000   // real-time event polling interval
#define PROFILE_DISPLAY_MS      2000   // profile card display time

// ─── NFC scan-gate constants ─────────────────────────────────────────────────
#define MIN_CARD_ID_LEN          8     // reject partial / noise reads
#define SCAN_COOLDOWN_MS       3500   // same-card lockout after accepted scan
#define CARD_CONFIRM_NEEDED      2    // consecutive reads before accepting

#define AP_SSID     "JJC_Attendance_Config"
#define AP_PASSWORD "ilovejjcenggworks"
#define SERVER_URL  "https://jjcenggworks.com"

String deviceId = "Attendance_Display_01";

// ─── Software clock ──────────────────────────────────────────────────────────
static uint8_t  clkH = 0, clkM = 0, clkS = 0;
static uint32_t clkEpoch = 0;   // Unix timestamp of last NTP sync (0 = unknown)

static void tickClock() {
    if (++clkS >= 60) { clkS = 0;
    if (++clkM >= 60) { clkM = 0;
    if (++clkH >= 24) { clkH = 0; if (clkEpoch) clkEpoch += 86400; }}}
    if (clkEpoch) clkEpoch++;
}
static String clockStr() {
    char b[12]; snprintf(b, sizeof(b), "%02d:%02d:%02d", clkH, clkM, clkS);
    return b;
}
// Returns "YYYY-MM-DD" from epoch or "" if clock not synced
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
    // Use real NTP date if clock is synced
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
    // Fallback before NTP sync
    return "--- AWAITING SYNC ---";
}

// ─── Globals ─────────────────────────────────────────────────────────────────
WiFiConfig             wifiConfig(AP_SSID, AP_PASSWORD);
WiFiManager            wifiManager;
AttendanceHTTPService  attService(SERVER_URL);
EmployeeProfileDisplay* empDisplay = nullptr;

static bool initialSyncDone = false;

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
// PENDING ATTENDANCE QUEUE
//
// DESIGN: Every NFC scan enqueues here FIRST — the HTTP upload happens
// separately via flushPending(). This is the core of the fast-tap architecture.
//
// pendingQueue holds records that are CONFIRMED locally (written to SD)
// but not yet ACKed by the server. flushPending() drains this at
// UPLOAD_FLUSH_MS cadence, sending UPLOAD_BATCH_SIZE records per cycle.
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
        Serial.println("[Queue] FULL — dropping oldest record");
        // Shift queue (lose oldest)
        for (int i = 0; i < 31; i++) pendingQueue[i] = pendingQueue[i+1];
        pendingQueue[31] = {empUid, nfcUid, clockType, ts, dt};
    }
}

// ── flushPending ──────────────────────────────────────────────────────────────
// Sends up to UPLOAD_BATCH_SIZE queued records to the server.
// Called on a timer from loop() — NEVER from handleNFCDetected().
//
// The batch limit keeps each flush cycle short enough that the NFC
// poller is not blocked for long. On a fast connection each POST
// takes ~300-600ms; 3 records = max ~2 seconds per cycle.
static void flushPending() {
    if (!wifiConfig.isConnected() || pendingCount == 0) return;

    int toSend = min(pendingCount, UPLOAD_BATCH_SIZE);
    Serial.printf("[Flush] Uploading %d/%d queued record(s)...\n", toSend, pendingCount);

    int remaining = 0;
    PendingRecord keep[32];

    for (int i = 0; i < pendingCount; i++) {
        if (i < toSend) {
            // Try to upload this record
            bool ok = attService.recordAttendance(
                pendingQueue[i].empUid, pendingQueue[i].nfcUid,
                deviceId, pendingQueue[i].clockType,
                pendingQueue[i].timestamp, pendingQueue[i].date);

            if (!ok) {
                // Upload failed — keep it for next cycle
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
            // Beyond batch limit — keep for next cycle
            keep[remaining++] = pendingQueue[i];
        }
    }

    for (int j = 0; j < remaining; j++) pendingQueue[j] = keep[j];
    pendingCount = remaining;
    Serial.printf("[Flush] Done. %d remain in queue.\n", pendingCount);
}

// ════════════════════════════════════════════════════════════════════════════
// SOCKET POLLER — unified real-time event handler
// ════════════════════════════════════════════════════════════════════════════
static void pollSocketEvents() {
    if (!wifiConfig.isConnected()) return;

    int empChanges = EmployeeSync::pollChanges(attService, String(SERVER_URL));
    if (empChanges > 0) {
        Serial.printf("[Socket] %d employee change(s) applied to SD cache\n", empChanges);
        if (currentState == STATE_DASHBOARD)
            updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                                  max(0, SDDatabase::countTodayCheckOuts()));
    }

    static double attLastTs = 0.0;

    char url[256];
    snprintf(url, sizeof(url),
             "%s/api/socket?action=poll&since=%.6f",
             SERVER_URL, attLastTs);

    HTTPClient http;
    http.setTimeout(5000);
    http.begin(url);
    http.addHeader("X-Client-Type", "ESP32");
    int code = http.GET();

    if (code != 200) {
        if (code > 0) Serial.printf("[Socket] Att-poll HTTP %d\n", code);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();
    if (body.length() == 0) return;

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, body) != DeserializationError::Ok) return;
    if (!doc["success"] | false) return;

    double serverTs = doc["timestamp"] | attLastTs;
    if (serverTs > attLastTs) attLastTs = serverTs;

    JsonArray events = doc["events"].as<JsonArray>();
    if (events.isNull() || events.size() == 0) return;

    Serial.printf("[Socket] %d event(s)\n", (int)events.size());

    bool statsNeedRefresh = false;

    for (JsonObject evt : events) {
        const char* evtName = evt["event"] | "";
        JsonObject  data    = evt["data"].as<JsonObject>();

        if (strcmp(evtName, "attendance_created") == 0 ||
            strcmp(evtName, "attendance_updated") == 0 ||
            strcmp(evtName, "attendance_update")  == 0 ||
            strcmp(evtName, "attendance_synced")  == 0) {

            statsNeedRefresh = true;

            const char* empName = data["employee_name"] | data["full_name"] | "";
            const char* cType   = data["clock_type"]    | "";
            const char* cTime   = data["clock_time"]    | "";

            if (strlen(empName) > 0 && strlen(cType) > 0 &&
                currentState == STATE_DASHBOARD) {

                String timeStr(cTime);
                int sp = timeStr.indexOf(' ');
                if (sp >= 0) timeStr = timeStr.substring(sp + 1);
                timeStr = timeStr.substring(0, 5);

                String label(cType);
                label.replace("_", " ");

                Serial.printf("[Socket] Z8 update: %s %s @ %s\n",
                              empName, cType, timeStr.c_str());
                updateLastScan(String(empName), label, timeStr);
            }
        }
    }

    if (statsNeedRefresh && currentState == STATE_DASHBOARD) {
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
    }
}

// ─── NTP Time Sync ────────────────────────────────────────────────────────────
void syncNTPTime() {
    if (!wifiConfig.isConnected()) return;
    Serial.println("[Time] Syncing NTP...");
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) {
        clkH     = timeinfo.tm_hour;
        clkM     = timeinfo.tm_min;
        clkS     = timeinfo.tm_sec;
        clkEpoch = (uint32_t)mktime(&timeinfo);
        SDDatabase::setDateProvider([]() -> String { return dateStr(); });
        Serial.printf("[Time] OK: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
                      clkH, clkM, clkS);
    } else {
        Serial.println("[Time] NTP failed — using software clock");
    }
}

// ─── Photo cache helper ───────────────────────────────────────────────────────
static String downloadAndCachePhoto(const EmployeeProfile& emp) {
    if (!SDDatabase::isReady() || emp.uid.length() == 0) return "";
    if (SDDatabase::hasPhoto(emp.uid)) return SDDatabase::photoPath(emp.uid);
    if (!wifiConfig.isConnected()) return "";

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
// seedTodayAttendanceFromServer
//
// Pulls today's attendance records from the server and writes any missing
// entries into the local SD CSV so resolveClockType() is correct even when:
//   • The device just booted and the SD CSV is empty for today.
//   • An employee clocked in on a DIFFERENT device (web portal, another reader).
//   • The device was offline and missed taps that were recorded elsewhere.
//
// Only called once per boot, inside triggerInitialSync(), AFTER NTP sync so
// dateStr() returns a valid "YYYY-MM-DD" string.
//
// The function is intentionally lightweight — it only writes rows that are
// genuinely absent from SD (idempotent), so calling it again is harmless.
// ════════════════════════════════════════════════════════════════════════════
static void seedTodayAttendanceFromServer() {
    if (!wifiConfig.isConnected()) return;
    if (!SDDatabase::isReady()) return;

    String today = dateStr();
    if (today.length() < 10) {
        Serial.println("[Seed] Skipping — clock not yet synced (no NTP date)");
        return;
    }

    Serial.println("[Seed] Fetching today's attendance from server: " + today);
    Serial.flush();

    int seeded = attService.fetchTodayAttendance(today);

    if (seeded > 0) {
        Serial.printf("[Seed] %d server record(s) seeded to SD — clock types are now accurate\n", seeded);
        // Update the dashboard stats to reflect the freshly seeded data
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
    } else {
        Serial.println("[Seed] SD already up-to-date with server (0 new rows)");
    }
    Serial.flush();
}

// ─── triggerInitialSync ───────────────────────────────────────────────────────
static void triggerInitialSync() {
    if (!wifiConfig.isConnected()) {
        Serial.println("[Sync] SKIP: no WiFi — SD-only mode active");
        return;
    }
    if (initialSyncDone) {
        Serial.println("[Sync] SKIP: already synced this session");
        return;
    }
    initialSyncDone = true;

    Serial.println("[Sync] WiFi available — syncing employees (SD-first)");
    Serial.printf("[Sync] Heap=%u  PSRAM=%u\n", ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.flush();

    bool hasCache = SDDatabase::isReady() &&
                    SD_MMC.exists("/employees") &&
                    SD_MMC.exists("/employees/sync_meta.json");
    Serial.printf("[Sync] SD cache: %s\n", hasCache ? "exists — incremental" : "empty — full sync");

    EmployeeSync::fullSyncIfNeeded(attService, !hasCache);

    // ── SEED TODAY'S ATTENDANCE FROM SERVER ───────────────────────────────
    // Must run AFTER fullSyncIfNeeded (employees must be in SD cache first)
    // and AFTER NTP is available (dateStr() must return "YYYY-MM-DD").
    // This ensures resolveClockType() is correct from the first tap of the day
    // even if employees clocked in elsewhere before this device booted.
    seedTodayAttendanceFromServer();

    // Flush any records that were queued before WiFi came up
    if (pendingCount > 0) flushPending();

    drawStaticUI();
    updateStatusDots(true, SDDatabase::isReady(), true);
    updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                          max(0, SDDatabase::countTodayCheckOuts()));
    Serial.println("[Sync] Complete");
    Serial.flush();
}

// ════════════════════════════════════════════════════════════════════════════
// handleNFCDetected  — FAST-TAP VERSION
//
// CRITICAL CHANGE from previous version:
//   ❌ OLD: attService.recordAttendance() called here → blocks loop for 1-3s
//   ✅ NEW: SD log written here (< 5ms) → record enqueued → returns immediately
//           Server upload happens in flushPending() on a 3s background timer.
//
// Tap-to-feedback latency: < 200ms (SD write + display render)
// Server upload latency:   ~3s after tap (first flush cycle)
//
// Flow:
//   1. Resolve employee UID from NFC card → SD mapping file
//   2. Load employee profile from SD (offline-capable)
//   3. If SD miss AND WiFi up: fetch from server + cache to SD
//   4. Ensure photo cached on SD
//   5. Determine next clock type
//   6. Log attendance to SD immediately  ← local record guaranteed
//   7. Enqueue for background upload     ← returns, does NOT block
//   8. Show profile card + RECORDED badge
// ════════════════════════════════════════════════════════════════════════════
static void handleNFCDetected(const String& cardIdentifier) {
    Serial.println("\n================================================");
    Serial.println("[NFC] Card detected: " + cardIdentifier);
    Serial.printf("[NFC] Heap=%u  PSRAM=%u  SD=%s  WiFi=%s  Queue=%d\n",
        ESP.getFreeHeap(), ESP.getFreePsram(),
        SDDatabase::isReady() ? "OK" : "NO",
        wifiConfig.isConnected() ? "OK" : "NO",
        pendingCount);
    Serial.println("================================================");
    Serial.flush();

    lastNFCUid = cardIdentifier;
    enterState(STATE_NFC_LOADING);
    empDisplay->showLoading();

    EmployeeProfile emp;
    bool fromCache = false;

    // ── STEP 1: Resolve employee UID from card ID ─────────────────────────
    String empUid = SDDatabase::loadUidForNfc(cardIdentifier);
    if (empUid.length() == 0 && SDDatabase::hasEmployeeProfile(cardIdentifier))
        empUid = cardIdentifier;
    Serial.printf("[STEP-1] empUid='%s'\n", empUid.c_str());

    // ── STEP 2: Load profile from SD (SD-FIRST, offline-capable) ─────────
    if (empUid.length() > 0 && SDDatabase::hasEmployeeProfile(empUid)) {
        if (SDDatabase::loadEmployeeProfile(empUid, emp)) {
            fromCache = true;
            Serial.printf("[STEP-2] SD hit: %s\n", emp.fullName.c_str());
        } else {
            Serial.println("[STEP-2] SD hit but load FAILED");
        }
    } else {
        Serial.println("[STEP-2] SD miss");
    }

    // ── STEP 3: Server fallback (only if SD miss) ─────────────────────────
    if (!fromCache) {
        if (!wifiConfig.isConnected()) {
            Serial.println("[STEP-3] Offline + unknown card — no log");
            empDisplay->showError("Unknown Card\n(Offline)");
            enterState(STATE_NFC_ERROR);
            return;
        }

        Serial.println("[STEP-3] Fetching from server...");
        bool granted = attService.authenticateNFC(cardIdentifier, deviceId, emp);
        if (!granted) {
            Serial.println("[STEP-3] Server denied — no log");
            empDisplay->showError(emp.hasData ? "Access Denied" : "Card Not Registered");
            enterState(STATE_NFC_ERROR);
            return;
        }

        if (SDDatabase::isReady() && emp.uid.length() > 0) {
            SDDatabase::saveEmployeeProfile(emp.uid, emp);
            SDDatabase::saveNfcMapping(cardIdentifier, emp.uid);
            Serial.println("[STEP-3] Cached to SD for offline use");
        }
    }

    // ── STEP 4: Photo ─────────────────────────────────────────────────────
    String photoPath = "";
    if (SDDatabase::isReady() && emp.uid.length() > 0) {
        if (SDDatabase::hasPhoto(emp.uid)) {
            String cand = SDDatabase::photoPath(emp.uid);
            if (SD_MMC.exists(cand)) {
                File f = SD_MMC.open(cand, FILE_READ);
                if (f && f.size() >= 100) { photoPath = cand; f.close(); }
            }
        }
        if (photoPath.length() == 0 && wifiConfig.isConnected())
            photoPath = downloadAndCachePhoto(emp);
    }
    Serial.printf("[STEP-4] photo='%s'\n", photoPath.c_str());

    // ── STEP 5: Determine clock type ──────────────────────────────────────
    String clockType = resolveClockType(emp.uid);
    emp.clockType = clockType;
    lastClockType = clockType;
    lastEmployee  = emp;
    Serial.printf("[STEP-5] clockType=%s\n", clockType.c_str());

    // ── STEP 6: Show profile card ─────────────────────────────────────────
    empDisplay->showEmployeeProfile(emp, photoPath);
    Serial.println("[STEP-6] Profile shown");

    // ── STEP 7: Log to SD immediately (guaranteed local record) ──────────
    String ts        = clockStr();    // "HH:MM:SS"
    String todayDate = dateStr();     // "YYYY-MM-DD" or ""

    if (SDDatabase::isReady()) {
        SDDatabase::logAttendance(ts, cardIdentifier, emp, clockType, deviceId);
        Serial.println("[STEP-7] SD logged — instant local record confirmed");
    }

    // ── STEP 8: Enqueue for background upload — DO NOT BLOCK HERE ─────────
    // This returns immediately. flushPending() will POST to the server on the
    // next UPLOAD_FLUSH_MS tick in loop(). Even if WiFi is down, the record is
    // safe on SD and will sync when connectivity is restored.
    enqueuePending(emp.uid, cardIdentifier, clockType, ts, todayDate);
    Serial.printf("[STEP-8] Enqueued for upload. Queue size: %d\n", pendingCount);

    // "RECORDED" badge over the photo
    empDisplay->showSuccess(emp.fullName);

    // Cooldown tracking
    lastAcceptedScanAt = millis();
    lastAcceptedCardId = cardIdentifier;

    Serial.println("[STEP-8] Done → NFC_PROFILE timer (tap complete in ~200ms)");
    Serial.println("================================================");
    Serial.flush();

    enterState(STATE_NFC_PROFILE);
}

// ─── setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    { unsigned long t0 = millis(); while (!Serial && millis()-t0 < 5000) delay(10); }
    delay(200);

    Serial.println();
    Serial.println("========================================");
    Serial.println("[Boot] JJC NFC Attendance System");
    Serial.printf("[Boot] %s  cores=%d  %dMHz\n",
                  ESP.getChipModel(), ESP.getChipCores(), getCpuFrequencyMhz());
    Serial.printf("[Boot] Heap=%u  PSRAM=%u\n", ESP.getFreeHeap(), ESP.getPsramSize());
    Serial.println("========================================");
    Serial.flush();

    // ── 1. TFT ────────────────────────────────────────────────────────────
    Serial.println("[Boot] 1/6 TFT...");
    if (!TFTDisplayManager::init(0)) {
        Serial.println("[Boot] TFT FAILED — halt");
        while (true) delay(2000);
    }
    dashboardInit();
    showLoadingAnimation(0, "Initializing...");
    empDisplay = new EmployeeProfileDisplay();
    showLoadingAnimation(15, "Display OK");
    delay(100);

    // ── 2. SD Card ────────────────────────────────────────────────────────
    Serial.println("[Boot] 2/6 SD...");
    showLoadingAnimation(20, "Mounting SD...");
    if (SDDatabase::begin()) {
        showLoadingAnimation(35, "SD Ready");
        Serial.println("[Boot] SD OK — offline mode available");
        SDDatabase::printInfo();
    } else {
        showLoadingAnimation(35, "SD FAILED");
        Serial.println("[Boot] SD FAILED — no local cache");
    }
    delay(100);

    // ── 3. NFC ────────────────────────────────────────────────────────────
    Serial.println("[Boot] 3/6 NFC...");
    showLoadingAnimation(40, "NFC Init...");
    if (!nfcInit()) {
        showLoadingAnimation(100, "NFC FAILED!");
        Serial.println("[Boot] NFC FAILED — halt");
        delay(1000); while (true) delay(2000);
    }
    showLoadingAnimation(60, "NFC Ready");
    Serial.println("[Boot] NFC OK");
    delay(100);

    // ── 4. WiFi ───────────────────────────────────────────────────────────
    Serial.println("[Boot] 4/6 WiFi...");
    showLoadingAnimation(65, "WiFi connecting...");
    wifiConfig.begin();
    if (wifiConfig.isConnected()) {
        showLoadingAnimation(80, ("WiFi: " + wifiConfig.getSSID()).c_str());
        Serial.println("[Boot] WiFi: " + wifiConfig.getSSID() +
                       "  IP=" + wifiConfig.getIPAddress());
        syncNTPTime();
    } else {
        showLoadingAnimation(80, "WiFi: AP mode (SD-only)");
        Serial.println("[Boot] WiFi not connected — running SD-only");
    }
    delay(100);

    // ── 5. Web portal ─────────────────────────────────────────────────────
    Serial.println("[Boot] 5/6 Portal...");
    showLoadingAnimation(85, "Web Portal...");
    wifiManager.init(deviceId, &wifiConfig, SERVER_URL, &attService);
    showLoadingAnimation(95, "Portal Ready");
    delay(100);

    // ── 6. Ready ──────────────────────────────────────────────────────────
    showLoadingAnimation(100, "System Ready!");
    delay(300);

    TFT_eSPI* t = dashboardGetTFT();
    t->fillScreen(TFTColors::BLACK);
    t->setTextColor(TFTColors::GREEN);
    t->setTextDatum(MC_DATUM);
    t->drawString("READY!", SCREEN_W/2, SCREEN_H/2, 4);
    t->setTextDatum(TL_DATUM);
    delay(400);

    drawStaticUI();
    updateStatusDots(wifiConfig.isConnected(), SDDatabase::isReady(), true);
    updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                          max(0, SDDatabase::countTodayCheckOuts()));
    enterState(STATE_DASHBOARD);

    Serial.flush();

    if (wifiConfig.isConnected()) {
        triggerInitialSync();
    } else {
        Serial.println("[Ready] SD-only mode — scanning active");
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
    static unsigned long lastUploadFlush = 0;   // ← NEW: background upload timer
    static unsigned long lastSocketPoll  = 0;
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
        triggerInitialSync();       // seeds today's attendance from server on first connect
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

    // ── Socket event poller ───────────────────────────────────────────────
    if (isConnected && currentState == STATE_DASHBOARD &&
        (now - lastSocketPoll >= SOCKET_POLL_MS)) {
        lastSocketPoll = now;
        pollSocketEvents();
    }

    // ── Background upload scheduler (CORE OF FAST-TAP FIX) ───────────────
    // Runs every UPLOAD_FLUSH_MS when:
    //   • WiFi is connected (server reachable)
    //   • There are records in the queue
    //   • System is on the dashboard (not mid-scan display)
    //     This last condition prevents the HTTP call from competing with
    //     the TFT render during the 2-second profile display window.
    if (isConnected && pendingCount > 0 &&
        currentState == STATE_DASHBOARD &&
        (now - lastUploadFlush >= UPLOAD_FLUSH_MS)) {
        lastUploadFlush = now;
        flushPending();

        // Refresh stats after upload so the dashboard counts stay live
        if (currentState == STATE_DASHBOARD) {
            updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                                  max(0, SDDatabase::countTodayCheckOuts()));
        }
    }

    // ── State machine ─────────────────────────────────────────────────────
    switch (currentState) {

        case STATE_NFC_PROFILE:
            if (stateElapsed() >= PROFILE_DISPLAY_MS) {
                enterState(STATE_DASHBOARD);
                drawStaticUI();
                updateStatusDots(isConnected, SDDatabase::isReady(), true);
                updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                                      max(0, SDDatabase::countTodayCheckOuts()));
                if (lastEmployee.hasData) {
                    String disp = lastClockType;
                    disp.replace("_", " ");
                    char ts[6]; snprintf(ts, sizeof(ts), "%02d:%02d", clkH, clkM);
                    updateLastScan(lastEmployee.fullName, disp, String(ts));
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
    if (currentState == STATE_DASHBOARD && (now - lastClock >= CLOCK_UPDATE_MS)) {
        lastClock = now; tick++;
        tickClock();
        pulseStatus(tick % 2);
        updateClock(clkH, clkM, clkS);
        if (tick % 60   == 0) updateDate(buildDateStr());
        if (tick % 3600 == 0 && isConnected) syncNTPTime();

        if (clkH == 0 && clkM == 0 && clkS == 0) {
            Serial.println("[Midnight] New day — resetting stats and UI");
            drawStaticUI();
            updateStatusDots(isConnected, SDDatabase::isReady(), true);
            updateAttendanceStats(0, 0);
            updateDate(buildDateStr());
            SDDatabase::setDateProvider([]() -> String { return dateStr(); });
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
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 100)) {
            nfcProcessCard(uid, uidLen);
            String cardId = (nfcData.length() > 0) ? nfcData : nfcUID;

            if ((int)cardId.length() < MIN_CARD_ID_LEN) {
                Serial.printf("[NFC] NOISE len=%d: '%s'\n",
                              (int)cardId.length(), cardId.c_str());
                cardConfirmed(cardId);

            } else if (!cardConfirmed(cardId)) {
                Serial.printf("[NFC] CONFIRMING: '%s'\n", cardId.c_str());

            } else if (cardId == lastAcceptedCardId &&
                       (now - lastAcceptedScanAt) < SCAN_COOLDOWN_MS) {
                // Same card resting — cooldown (silent)

            } else if (currentState != STATE_DASHBOARD) {
                Serial.printf("[NFC] WAITING: '%s' (not on dashboard)\n", cardId.c_str());

            } else {
                Serial.println("[NFC] ACCEPTED: " + cardId);
                _lastRawCard   = cardId;
                _cardConfirmCt = CARD_CONFIRM_NEEDED;
                handleNFCDetected(cardId);
            }

        } else {
            if (_cardConfirmCt > 0) { _lastRawCard = ""; _cardConfirmCt = 0; }
        }
    }
}