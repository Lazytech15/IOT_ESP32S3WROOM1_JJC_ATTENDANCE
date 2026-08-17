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
//                       Auto-reseed has been removed; seeding is manual-only.
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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ─── Timing constants ────────────────────────────────────────────────────────
#define NFC_POLL_INTERVAL_MS      30    // loop check cadence — how often we call readPassiveTargetID
                                        // (the read itself still uses a 50ms RF timeout inside)
#define CLOCK_UPDATE_MS         1000
#define STATS_REFRESH_MS       30000
#define WIFI_RETRY_MS          20000
#define WIFI_RECONNECT_RESYNC_COOLDOWN_MS  30000   // debounce for flapping WiFi (see loop())

// ── RECONCILE POLLER ──────────────────────────────────────────────────────────
// Periodically re-runs seedTodayAttendanceFromServer() (PASS 1-3, including the
// server-delete reconciliation) so a portal-side deletion reaches the device
// without waiting for boot / WiFi-reconnect / manual "Reseed Today". Modeled on
// the web portal's PollingManager (websocket/polling-manager.jsx): fixed base
// interval, exponential backoff on failure, reset to base on success.
#define RECONCILE_POLL_BASE_MS     15000    // 15s, same cadence as the portal poller
#define RECONCILE_POLL_MAX_MS     120000    // cap backoff at 2 minutes
#define SCREEN_TIMEOUT_MS     300000   // 5 minutes of inactivity → backlight off

// ── PROFILE DISPLAY ───────────────────────────────────────────────────────────
// How long the employee photo/badge stays on screen after a scan.
// Normal:   2000ms — comfortable read time during quiet periods.
// Peak:      800ms — fast turnover during rush-hour lineups.
// The display auto-shortens during peak hours so the queue moves faster.
// A new card from a DIFFERENT employee also short-circuits the wait immediately.
#define PROFILE_DISPLAY_MS      2000
#define PROFILE_DISPLAY_PEAK_MS  800   // rush-hour display time

// ── UPLOAD SCHEDULER ──────────────────────────────────────────────────────────
#define UPLOAD_FLUSH_MS      600000    // upload cadence: 10 minutes (was 500ms — was causing freeze after every scan)
#define UPLOAD_BATCH_SIZE          1    // one HTTP call per cycle → predictable latency
#define SOCKET_POLL_MS       3600000   // 1 hour — prevents clock freeze on dashboard

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
    // No cap — after a long sleep we must jump to the correct time.
    // Anchor lastClock so the next call starts from a clean slate.
    lastClock = now - (elapsed % 1000);   // keep sub-second remainder
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

// ─── Screen timeout ───────────────────────────────────────────────────────────
static unsigned long lastActivityMs  = 0;   // last time the screen was "touched"
static bool          screenIsOff     = false;

static bool          wasConnectedGlobal = false;  // tracks WiFi state across loop()

// Set true by wakeScreen() so loop()'s clock branch resets lastClock to now,
// preventing a huge catch-up burst of ticks after a long standby.
static bool g_clockNeedsReset = false;

// Re-sync the software clock from the ESP32 RTC (no network needed).
// The RTC keeps ticking during standby / screen-off, so this instantly
// corrects any drift that accumulated while the display was blanked.
static void resyncClockFromRTC() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        clkH     = timeinfo.tm_hour;
        clkM     = timeinfo.tm_min;
        clkS     = timeinfo.tm_sec;
        clkEpoch = (uint32_t)mktime(&timeinfo);
        Serial.printf("[Clock] RTC resync on wake: %02d:%02d:%02d\n",
                      clkH, clkM, clkS);
    } else {
        // RTC unavailable (no NTP yet) — keep rolling from wherever we are
        Serial.println("[Clock] RTC unavailable on wake, keeping software clock");
    }
}

static void wakeScreen() {
    if (screenIsOff) {
        screenIsOff = false;

        // Resync software clock from RTC so the display jumps to the correct
        // time immediately rather than catching up second-by-second.
        resyncClockFromRTC();
        g_clockNeedsReset = true;   // anchor lastClock to now in loop()

        TFTDisplayManager::backlightOn();
        drawStaticUI();
        updateClock(clkH, clkM, clkS);
        updateDate(buildDateStr());
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
        Serial.println("[Screen] Wake — screen restored");
    }
    lastActivityMs = millis();
}
static void resetScreenTimer() { lastActivityMs = millis(); }

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

// ── Deferred stats refresh flag ──────────────────────────────────────────────
// Set to true when the state machine returns to dashboard after a scan.
// loop() reads this on the NEXT tick and refreshes attendance stats (which
// require SD file scans) WITHOUT blocking the NFC poll or clock on the
// same tick as the drawStaticUI() redraw.
static bool g_pendingStatsRefresh = false;
// Set when reconcileTodayAttendance() removes a row (i.e. an admin deleted
// an attendance entry from the portal). We don't track which employee/type
// the removed row belonged to, so on the next dashboard tick we clear both
// Clock-In/Clock-Out name strips rather than risk leaving a deleted
// person's name on screen.
static bool g_pendingLastScanClear = false;

// ── Midnight rollover state ───────────────────────────────────────────────────
// See purgeSyncedAttendanceDate() and the rollover block in loop().
static String g_lastSeenDate         = "";   // last dateStr() observed — detects the day changing
static String g_cleanupPendingDate   = "";   // yesterday's date, waiting for pendingCount==0 to purge
static bool   g_weeklyRefreshPending = false; // set on a Monday rollover; run once dashboard is idle

// ── Scan-ahead buffer ────────────────────────────────────────────────────────
// During rush hour the profile display is showing (STATE_NFC_PROFILE) but the
// next person has already tapped. We buffer that card so the state machine can
// process it the instant the display clears — zero idle gap between employees.
static String        _nextPendingCard   = "";   // buffered while in NFC_PROFILE/ERROR

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
//
// Session time windows (device-local 24h), aligned with the peak windows in
// isPeakHour() (noon out ~12:00, afternoon out ~17:00):
//   morning:   00:00 – 11:59
//   afternoon: 12:00 – 16:59
//   evening:   17:00 – 23:59
//
// FIX: resolveClockType() used to ignore the clock entirely and just fill
// slots in fixed order (morning_in -> morning_out -> afternoon_in -> ...).
// A late/off-schedule tap (e.g. 1:47 PM) got labelled "morning_in" simply
// because morning was the first empty slot — not because it was morning.
// We now pick the session that matches the CURRENT time first, and only
// fall back to old "first empty slot" scanning for edge cases (e.g. the
// current session's in/out are both already filled — a duplicate tap).
static int _currentSessionIndex() {
    uint16_t nowMin = (uint16_t)clkH * 60 + clkM;
    if (nowMin < 12*60) return 0;       // morning
    if (nowMin < 17*60) return 1;       // afternoon
    return 2;                            // evening
}

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

    int cur = _currentSessionIndex();
    if (hasIn[cur] && !hasOut[cur]) return String(SESSIONS[cur]) + "_out";
    if (!hasIn[cur])                 return String(SESSIONS[cur]) + "_in";

    // Current session already fully clocked (duplicate tap) — fall back
    // to the first genuinely open slot elsewhere in the day.
    for (int si = 0; SESSIONS[si]; si++) {
        if (hasIn[si] && !hasOut[si]) return String(SESSIONS[si]) + "_out";
    }
    for (int si = 0; SESSIONS[si]; si++) {
        if (!hasIn[si]) return String(SESSIONS[si]) + "_in";
    }
    return String(SESSIONS[cur]) + "_out";  // everything filled — re-log current session's out

#else
    if (!SDDatabase::isReady()) return String(SESSIONS[_currentSessionIndex()]) + "_in";

    String todayLog = SDDatabase::loadAttendanceToday(empUid);

    if (todayLog.length() == 0) {
        bool anyRecord = SDDatabase::hasCheckedInToday(empUid);
        int cur = _currentSessionIndex();
        return anyRecord ? (String(SESSIONS[cur]) + "_out")
                          : (String(SESSIONS[cur]) + "_in");
    }

    bool hasIn[3]  = {false, false, false};
    bool hasOut[3] = {false, false, false};
    for (int si = 0; SESSIONS[si]; si++) {
        String inKey  = String(SESSIONS[si]) + "_in";
        String outKey = String(SESSIONS[si]) + "_out";
        if (todayLog.indexOf(inKey)  >= 0) hasIn[si]  = true;
        if (todayLog.indexOf(outKey) >= 0) hasOut[si] = true;
    }

    int cur = _currentSessionIndex();
    if (hasIn[cur] && !hasOut[cur]) return String(SESSIONS[cur]) + "_out";
    if (!hasIn[cur])                 return String(SESSIONS[cur]) + "_in";

    // Current session already fully clocked (duplicate tap) — fall back
    // to the first genuinely open slot elsewhere in the day.
    for (int si = 0; SESSIONS[si]; si++) {
        if (hasIn[si] && !hasOut[si]) return String(SESSIONS[si]) + "_out";
    }
    for (int si = 0; SESSIONS[si]; si++) {
        if (!hasIn[si]) return String(SESSIONS[si]) + "_in";
    }
    return String(SESSIONS[cur]) + "_out";  // everything filled — re-log current session's out
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// PENDING ATTENDANCE QUEUE  (in-memory + SD-backed)
//
// Every NFC scan writes to SD immediately, then adds to this queue.
// flushPending() drains the queue to the server in the background.
// During peak hours the flush is skipped — records stay on SD.
//
// SD-backed: the queue is mirrored to /attendance/pending_queue.json on every
// change (enqueue, successful upload, drop-oldest) and reloaded on boot. This
// is separate from the per-day attendance CSV — that's the permanent record
// of the tap; this file is purely "what still needs to reach the server."
// A record only leaves this file once recordAttendance() gets a confirmed
// OK from the server — so a reboot or power loss mid-day can't silently
// lose an unsynced scan the way the in-memory-only queue used to.
// ════════════════════════════════════════════════════════════════════════════
#define PENDING_QUEUE_PATH "/attendance/pending_queue.json"

struct PendingRecord {
    String empUid, nfcUid, clockType, timestamp, date;
};
static PendingRecord pendingQueue[32];
static int           pendingCount = 0;

// Overwrites PENDING_QUEUE_PATH with the current in-memory queue.
// Called after every change so the SD copy never falls behind. 32 short
// records is a small write (well under a second) — fine to do inline.
static void savePendingQueueToSD() {
    if (!SDDatabase::isReady()) return;

    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < pendingCount; i++) {
        JsonObject o = arr.createNestedObject();
        o["empUid"]    = pendingQueue[i].empUid;
        o["nfcUid"]    = pendingQueue[i].nfcUid;
        o["clockType"] = pendingQueue[i].clockType;
        o["timestamp"] = pendingQueue[i].timestamp;
        o["date"]      = pendingQueue[i].date;
    }

    File f = SD_MMC.open(PENDING_QUEUE_PATH, FILE_WRITE);
    if (!f) {
        Serial.println("[Queue] WARNING: could not open pending_queue.json for write");
        return;
    }
    serializeJson(doc, f);
    f.close();
}

// Called once at boot (after SD is ready, before scanning starts) to restore
// whatever didn't make it to the server before the last shutdown/reboot.
static void loadPendingQueueFromSD() {
    if (!SDDatabase::isReady()) return;
    if (!SD_MMC.exists(PENDING_QUEUE_PATH)) return;

    File f = SD_MMC.open(PENDING_QUEUE_PATH, FILE_READ);
    if (!f) return;

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[Queue] pending_queue.json parse failed: %s — starting empty\n",
                      err.c_str());
        return;
    }

    JsonArray arr = doc.as<JsonArray>();
    pendingCount = 0;
    for (JsonObject o : arr) {
        if (pendingCount >= 32) break;
        pendingQueue[pendingCount++] = {
            String((const char*)(o["empUid"]    | "")),
            String((const char*)(o["nfcUid"]    | "")),
            String((const char*)(o["clockType"] | "")),
            String((const char*)(o["timestamp"] | "")),
            String((const char*)(o["date"]      | ""))
        };
    }
    if (pendingCount > 0) {
        Serial.printf("[Queue] Restored %d unsynced record(s) from SD (survived reboot)\n",
                      pendingCount);
    }
}

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
    savePendingQueueToSD();   // mirror to SD immediately — survives reboot from here on
}

// Removes a matching not-yet-uploaded record from the pending queue.
// Called by the portal's "Delete row" action (via g_pendingQueueRemover, see
// WiFiManager.h) so that deleting a record which hasn't synced to the server
// yet actually sticks — otherwise the queue entry survives the delete,
// survives a reboot, and flushPending() uploads it later anyway, silently
// recreating the row the admin just deleted.
// Matches on empUid + clockType + date, and on timeOnly too when the caller
// has it (portal deletes always do); an empty timeOnly matches any time for
// that empUid/clockType/date, which is only used defensively.
static bool removeFromPendingQueue(const String& empUid, const String& clockType,
                                    const String& date, const String& timeOnly) {
    for (int i = 0; i < pendingCount; i++) {
        if (pendingQueue[i].empUid    != empUid)    continue;
        if (pendingQueue[i].clockType != clockType) continue;
        if (pendingQueue[i].date      != date)      continue;
        if (timeOnly.length() > 0 && pendingQueue[i].timestamp != timeOnly) continue;

        Serial.printf("[Queue] Portal delete: purging matching pending record "
                      "(%s %s %s %s)\n", empUid.c_str(), clockType.c_str(),
                      date.c_str(), pendingQueue[i].timestamp.c_str());
        for (int j = i; j < pendingCount - 1; j++) pendingQueue[j] = pendingQueue[j + 1];
        pendingCount--;
        savePendingQueueToSD();
        return true;
    }
    return false;
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
    savePendingQueueToSD();   // mirror the drained/retry-trimmed queue back to SD —
                              // confirmed-uploaded records are now gone from the file too
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
// ════════════════════════════════════════════════════════════════════════════
// Returns false only on a genuine network/transport failure (used by the
// background poller below to back off). A day with legitimately 0 rows to
// seed/remove still returns true — that's success, not an error.
static bool seedTodayAttendanceFromServer() {
    if (!wifiConfig.isConnected()) return false;
    if (!SDDatabase::isReady()) return false;

    String today = dateStr();
    if (today.length() < 10) {
        Serial.println("[Seed] Skipping — clock not yet synced");
        return false;
    }

    Serial.println("[Seed] === Seeding today's attendance: " + today + " ===");
    int seeded = 0;

    // Suspend SDLogger's per-line SD_MMC file write for the duration of the
    // three passes below — see SDLogger::suspendSDWrite() for why. Serial
    // output (and the CSV/reconcile results themselves) are unaffected;
    // this only skips writing the noisy per-employee trace lines to the SD
    // debug log file. Always resumed via the RAII-style guard so an early
    // return (e.g. PASS 1 issues) can't leave it stuck suspended.
    SDLogger::suspendSDWrite(true);
    struct ResumeSDWriteOnExit {
        ~ResumeSDWriteOnExit() { SDLogger::suspendSDWrite(false); }
    } resumeSDWriteGuard;

    // PASS 1: ESP32-sync snapshot (fetches the CURRENT server picture and
    // refreshes /attendance/esp32_<date>.json). This MUST run before PASS 2
    // below — PASS 2 seeds the CSV from that same snapshot file, and running
    // it first (as this used to) meant PASS 2 seeded from LAST cycle's
    // snapshot, i.e. from data that could already be stale/deleted on the
    // server. Fetching fresh first means a server-side deletion is already
    // reflected in the snapshot before anything reads it, so a fully-deleted
    // record (an employee's only row for the day) never gets re-seeded in
    // the first place instead of relying entirely on reconciliation to undo it.
    int syncResult = attService.fetchTodayAttendanceEsp32(today);
    if (syncResult > 0) {
        seeded += syncResult;
        Serial.printf("[Seed] PASS 1: %d row(s) from esp32-sync\n", syncResult);
    }

    // PASS 2: Local SD snapshot seeder — NOT a network call. Reads the
    // snapshot PASS 1 just refreshed and seeds any missing CSV rows from it.
    // A -1 here means that local file was empty/corrupt (e.g. PASS 1 above
    // failed, offline) — NOT a WiFi/server failure — so it must never abort
    // PASS 3/3B or count against the poller's backoff.
    int rawResult = attService.seedCsvFromRawAttendance(today);
    if (rawResult > 0) {
        seeded += rawResult;
        Serial.printf("[Seed] PASS 2: %d new row(s) seeded\n", rawResult);
    } else if (rawResult == 0) {
        Serial.println("[Seed] PASS 2: 0 new rows (up-to-date or no data)");
    } else {
        Serial.println("[Seed] PASS 2: local snapshot not ready yet (normal if PASS 1 failed) — continuing");
    }

    // PASS 3: Reconciliation — removes local rows the server no longer has.
    // PASS 1/2 above are purely additive (seed-if-missing); neither one ever
    // notices when a record was deleted server-side, so the local CSV would
    // otherwise keep phantom rows forever. This pass diffs local vs. server
    // and drops anything stale. Only touches employees the server actually
    // responded about this call, so it never wipes out a tap that's just
    // waiting to upload (see reconcileTodayAttendance() doc comment).
    int removedResult = attService.reconcileTodayAttendance(today);
    if (removedResult > 0) {
        Serial.printf("[Seed] PASS 3: %d stale row(s) removed (deleted server-side)\n",
                      removedResult);
        // A row was deleted on the portal — the last-scan name strip on the
        // TFT may currently be showing that person even though the stats
        // card above it is about to update. Clear it on the next tick.
        g_pendingLastScanClear = true;
    }

    // PASS 3B: Live-record reconciliation — makes the server the absolute
    // source of truth for today's CSV, catching what PASS 3 misses.
    // PASS 3 only checks employees the esp32-sync AGGREGATION endpoint still
    // returns for today; if a deleted record was an employee's ONLY row for
    // the day (or was a SNAP_SEED/SERVER_SEED row, which never gets a
    // server-ID map entry), PASS 3 (and the old ID-map-only pass) can't see
    // it, so it survives every cycle and every reboot. This pass instead
    // diffs the ENTIRE CSV against the server's live, non-aggregated record
    // list — see reconcileAgainstLiveRecords()'s doc comment for the full
    // explanation and the safety rule that still protects a local NFC tap
    // that hasn't uploaded yet.
    int removedById = attService.reconcileAgainstLiveRecords(today);
    if (removedById > 0) {
        Serial.printf("[Seed] PASS 3B: %d stale row(s) removed (live-record diff)\n",
                      removedById);
        g_pendingLastScanClear = true;
    }

    // Don't touch the TFT from here — this function now runs on Core 0
    // (see seedWorkerTask) and loop() on Core 1 may be mid-repaint. Just
    // flag it; loop() picks this up on its next DASHBOARD tick, same
    // mechanism already used for post-scan stats refresh.
    g_pendingStatsRefresh = true;
    Serial.printf("[Seed] === Seed complete: %d new, %d removed ===\n",
                  seeded, max(0, removedResult) + max(0, removedById));
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// purgeSyncedAttendanceDate  — midnight cleanup for a PAST date only
//
// Deletes the CSV + server_id map + esp32-sync snapshot for a date that has
// already rolled over (never "today"). Only called once the caller has
// confirmed pendingCount == 0 — i.e. nothing from that date is still
// waiting to reach the server. If a record failed to upload, it stays in
// pendingQueue (see flushPending()'s RETRY-later path), so pendingCount==0
// is a true "everything from that date made it out" signal, not a guess.
// ════════════════════════════════════════════════════════════════════════════
static void purgeSyncedAttendanceDate(const String& dateToPurge) {
    if (dateToPurge.length() != 10) return;  // guard against a malformed/empty date

    String csvPath  = "/attendance/" + dateToPurge + ".csv";
    String mapPath  = "/attendance/server_ids_" + dateToPurge + ".json";
    String snapPath = "/attendance/esp32_" + dateToPurge + ".json";

    bool csvRemoved  = SD_MMC.exists(csvPath)  ? SD_MMC.remove(csvPath)  : true;
    bool mapRemoved  = SD_MMC.exists(mapPath)  ? SD_MMC.remove(mapPath)  : true;
    bool snapRemoved = SD_MMC.exists(snapPath) ? SD_MMC.remove(snapPath) : true;

    Serial.printf("[Rollover] Purged %s — csv=%s map=%s snap=%s\n",
                  dateToPurge.c_str(),
                  csvRemoved  ? "OK" : "FAIL",
                  mapRemoved  ? "OK" : "FAIL",
                  snapRemoved ? "OK" : "FAIL");
}

// ── Seed worker — runs seedTodayAttendanceFromServer() on Core 0 ──────────────
// seedTodayAttendanceFromServer() does 2-3 paginated HTTP calls and was
// measured taking 5-10s against ~43 employees. Called directly from loop()
// (Core 1) it used to freeze NFC polling and the clock for that whole
// window — exactly when a WiFi hiccup-then-reconnect during a busy rush
// would trigger it. This task moves it off Core 1, same pattern as
// uploadWorkerTask. Skipped entirely during peak hour, matching every
// other background job in this codebase (upload flush, photo sync, etc).
static TaskHandle_t      g_seedTask    = nullptr;
static SemaphoreHandle_t g_seedTrigger = nullptr;  // binary — signaled to request a seed run
volatile bool            g_lastSeedOk  = true;      // read by the reconcile poller for backoff
volatile bool            g_seedRunning = false;      // true while a seed pass is in flight
volatile unsigned long   g_seedFinishedAtMs = 0;      // millis() when the last pass completed

static void seedWorkerTask(void* /*param*/) {
    for (;;) {
        if (xSemaphoreTake(g_seedTrigger, portMAX_DELAY) == pdTRUE) {
            if (isPeakHour()) {
                Serial.println("[Seed] Skipped — peak hour rush in progress");
                continue;
            }
            g_seedRunning     = true;
            g_lastSeedOk      = seedTodayAttendanceFromServer();
            g_seedFinishedAtMs = millis();
            g_seedRunning     = false;
        }
    }
}

// Request a seed run without blocking the caller. Before the worker task
// exists (i.e. the one-time boot call in setup(), before task creation),
// falls back to running it synchronously — safe at that point since NFC
// scanning isn't active yet.
static void requestSeedToday() {
    if (g_seedRunning) {
        Serial.println("[Seed] Request skipped — a seed pass is already in flight");
        return;
    }
    if (g_seedTrigger) {
        xSemaphoreGive(g_seedTrigger);
    } else {
        g_seedRunning = true;
        seedTodayAttendanceFromServer();
        g_seedRunning = false;
    }
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

    // Seed today's attendance from server — async once the seed worker task
    // exists (runtime reconnects); synchronous on the one-time boot call,
    // before any task or NFC scanning is up yet.
    requestSeedToday();

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
// NFC WORKER TASK  — runs on Core 0 so loop() on Core 1 is never blocked.
//
// Architecture:
//   loop()   — detects card, posts cardId to g_nfcQueue, returns immediately.
//   nfcWorkerTask() — picks up the cardId, runs all SD/HTTP/display work,
//                     then signals completion via g_nfcDone semaphore.
//
// Shared state rules:
//   • Only the worker task writes to currentState / lastEmployee / lastClockType.
//   • TFT calls (updateClock, drawStaticUI) stay on Core 1 inside loop();
//     the worker only calls empDisplay->show*() which uses the same TFT —
//     this is safe because the worker and loop() are serialised by the fact
//     that loop() does not call any TFT function while currentState ==
//     STATE_NFC_LOADING (the NFC poll gate already blocks it).
// ════════════════════════════════════════════════════════════════════════════

// Queue depth 1 — we only process one scan at a time.
// A second scan while the worker is busy gets buffered in _nextPendingCard
// and fired after the worker signals done.
static QueueHandle_t   g_nfcQueue   = nullptr;
static SemaphoreHandle_t g_nfcDone  = nullptr;
static TaskHandle_t    g_nfcTask    = nullptr;

// Card ID buffer written by loop(), read by the worker task.
// Max NFC data string is ~32 chars; 64 is plenty.
static char g_nfcCardBuf[64] = {0};

// ── Upload task — runs flushPending() on Core 0 ───────────────────────────────
// flushPending() calls recordAttendance() which is an HTTP POST that can
// block for up to 8 seconds. Running it in loop() (Core 1) freezes NFC polling
// and the clock. This task runs it on Core 0 so loop() is never blocked.
static TaskHandle_t      g_uploadTask   = nullptr;
static SemaphoreHandle_t g_uploadTrigger = nullptr;  // binary — loop() signals, task wakes

static void uploadWorkerTask(void* /*param*/) {
    for (;;) {
        // Wait for loop() to signal that an upload is needed
        if (xSemaphoreTake(g_uploadTrigger, portMAX_DELAY) == pdTRUE) {
            // Drain the ENTIRE queue, not just one record.
            // UPLOAD_BATCH_SIZE = 1 means flushPending() sends one record per
            // call, so we loop here until all pending records are uploaded.
            // This is critical when screen-off fires with multiple queued scans
            // (e.g. morning_in + morning_out) — the binary semaphore would
            // otherwise only wake us once and leave the 2nd record stranded.
            while (wifiConfig.isConnected() && pendingCount > 0 && !isPeakHour()) {
                flushPending();
                if (pendingCount > 0) delay(200);  // brief pause between HTTP calls
            }
        }
    }
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
static void nfcWorkerBody(const String& cardIdentifier) {
    // ── Wake display / refresh clock immediately ──────────────────────────
    if (screenIsOff) {
        wakeScreen();   // full restore: backlight + drawStaticUI + clock
    } else {
        // Screen already on — repaint clock right now before any blocking
        // SD / server work, so the display never appears frozen mid-scan
        updateClock(clkH, clkM, clkS);
        updateDate(buildDateStr());
    }

    Serial.println("\n================================================");
    Serial.println("[NFC] Card: " + cardIdentifier);
    Serial.printf("[NFC] Heap=%u PSRAM=%u SD=%s WiFi=%s Q=%d Peak=%s\n",
        ESP.getFreeHeap(), ESP.getFreePsram(),
        SDDatabase::isReady() ? "OK" : "NO",
        wifiConfig.isConnected() ? "OK" : "NO",
        pendingCount,
        isPeakHour() ? "YES" : "NO");

    lastNFCUid = cardIdentifier;
    // Note: enterState(STATE_NFC_LOADING) and showLoading() are called by
    // handleNFCDetected() (the dispatcher) before posting to the queue,
    // so loop() sees the state change immediately without waiting for the worker.

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
    yield();   // let background tasks breathe between heavy steps

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
    yield();   // photo lookup can be slow — yield before clock type resolve

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

        // During peak, skip the SD countToday calls — they scan the file and
        // add ~5-10ms per tap. Stats will catch up on the next 30s refresh.
        if (!isPeakHour()) {
            int ins  = max(0, SDDatabase::countTodayCheckIns());
            int outs = max(0, SDDatabase::countTodayCheckOuts());
            uint64_t freeMB = SDDatabase::freeBytes() / 1048576;
            String statsJson = "{\"ins\":" + String(ins)
                             + ",\"outs\":" + String(outs)
                             + ",\"free_mb\":" + String((int)freeMB)
                             + ",\"wifi\":true}";
            wifiManager.broadcastEvent("stats", statsJson);
        }
    }

    empDisplay->showSuccess(emp.fullName);
    lastAcceptedScanAt = millis();
    lastAcceptedCardId = cardIdentifier;

    Serial.println("[NFC] Done → STATE_NFC_PROFILE (2s timer)");
    enterState(STATE_NFC_PROFILE);
}

// ── FreeRTOS task: wraps nfcWorkerBody, runs on Core 0 ───────────────────────
static void nfcWorkerTask(void* /*param*/) {
    for (;;) {
        // Block until loop() posts a card ID into the queue
        char buf[64] = {0};
        if (xQueueReceive(g_nfcQueue, buf, portMAX_DELAY) == pdTRUE) {
            nfcWorkerBody(String(buf));
        }
        // Signal loop() that we are idle and ready for the next card
        xSemaphoreGive(g_nfcDone);
    }
}

// ── Non-blocking dispatcher: called from loop() ───────────────────────────────
// Posts the card ID to the worker queue and returns immediately.
// loop() continues ticking the clock and polling NFC while the worker runs.
static void handleNFCDetected(const String& cardIdentifier) {
    if (!g_nfcQueue || !g_nfcDone) {
        // Fallback: queue not initialised yet (shouldn't happen after setup)
        nfcWorkerBody(cardIdentifier);
        return;
    }

    // If the worker is still busy (semaphore not given yet), buffer the card
    // — _nextPendingCard already handles this in the NFC poll gate, so we
    // just drop here to avoid queue overflow.
    if (uxQueueMessagesWaiting(g_nfcQueue) > 0) {
        Serial.println("[NFC] Worker busy — card buffered by scan-ahead");
        return;
    }

    // Copy card ID into the shared buffer and post to queue
    strncpy(g_nfcCardBuf, cardIdentifier.c_str(), sizeof(g_nfcCardBuf) - 1);
    g_nfcCardBuf[sizeof(g_nfcCardBuf) - 1] = '\0';

    // enterState here so loop() sees NFC_LOADING immediately (NFC poll gate)
    enterState(STATE_NFC_LOADING);
    empDisplay->showLoading();

    xQueueSend(g_nfcQueue, g_nfcCardBuf, 0);
    Serial.println("[NFC] Card posted to worker: " + cardIdentifier);
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
        // Restore any records that hadn't reached the server before the
        // last shutdown/reboot — see loadPendingQueueFromSD() for details.
        // Must happen before the NFC worker task starts (step 7 below) so
        // nothing new can enqueue ahead of the restored records.
        loadPendingQueueFromSD();
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

    // Let the portal's "Delete row" action purge a matching not-yet-uploaded
    // record from the pending upload queue too (see g_pendingQueueRemover's
    // doc comment in WiFiManager.h for why this is needed).
    g_pendingQueueRemover = removeFromPendingQueue;

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
    lastActivityMs = millis();        // start screen timeout countdown from boot
    wasConnectedGlobal = wifiConfig.isConnected();  // sync state so loop() never fires a false reconnect event
    if (wifiConfig.isConnected()) {
        triggerInitialSync();
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
    } else {
        Serial.println("[Ready] SD-only — scanning active");
    }

    // ── 7. NFC worker task (Core 0) ──────────────────────────────────────────
    // loop() runs on Core 1 (Arduino default). The NFC worker runs on Core 0
    // so SD/HTTP work never blocks the clock, display, or NFC poll in loop().
    g_nfcQueue = xQueueCreate(1, sizeof(g_nfcCardBuf));   // depth-1 queue
    g_nfcDone  = xSemaphoreCreateBinary();
    xSemaphoreGive(g_nfcDone);   // start in "idle" state
    xTaskCreatePinnedToCore(
        nfcWorkerTask,   // task function
        "nfcWorker",     // name
        8192,            // stack (bytes) — enough for JSON + HTTP
        nullptr,         // parameter
        2,               // priority (higher than loop's 1)
        &g_nfcTask,      // handle
        0                // Core 0
    );
    Serial.println("[Ready] NFC worker task started on Core 0");

    // ── 8. Upload worker task (Core 0) ────────────────────────────────────────
    // Runs flushPending() (HTTP POST) off Core 1 so loop() is never blocked.
    // loop() signals this task via g_uploadTrigger when an upload is needed.
    g_uploadTrigger = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(
        uploadWorkerTask,  // task function
        "uploadWorker",    // name
        8192,              // stack — enough for HTTP + JSON
        nullptr,           // parameter
        1,                 // priority (same as loop, lower than nfcWorker)
        &g_uploadTask,     // handle
        0                  // Core 0 — keeps HTTP off Core 1
    );
    Serial.println("[Ready] Upload worker task started on Core 0");

    // ── 9. Seed worker task (Core 0) ──────────────────────────────────────────
    // Runs seedTodayAttendanceFromServer() (2-3 paginated HTTP calls, 5-10s)
    // off Core 1 so a WiFi-reconnect mid-rush or a portal "Reseed Today"
    // click never freezes NFC polling. loop() signals via g_seedTrigger.
    g_seedTrigger = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(
        seedWorkerTask,    // task function
        "seedWorker",      // name
        8192,              // stack — enough for HTTP + JSON
        nullptr,           // parameter
        1,                 // priority (same as uploadWorker)
        &g_seedTask,       // handle
        0                  // Core 0 — keeps HTTP off Core 1
    );
    Serial.println("[Ready] Seed worker task started on Core 0");

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
    static unsigned long reconcilePollRateMs = RECONCILE_POLL_BASE_MS;
    static int            reconcileErrorCount = 0;
    static uint8_t       tick            = 0;
    unsigned long now = millis();

    wifiConfig.handleClient();
    wifiManager.handleClient();

    // ── Screen timeout ────────────────────────────────────────────────────
    if (!screenIsOff &&
        currentState == STATE_DASHBOARD &&
        (now - lastActivityMs >= SCREEN_TIMEOUT_MS)) {
        screenIsOff = true;
        TFTDisplayManager::backlightOff();
        TFTDisplayManager::clearScreen(0x0000);
        Serial.println("[Screen] Timeout — screen blanked");

        // ── Trigger upload immediately on screen-off ──────────────────────
        // Instead of waiting up to 10 minutes for the upload timer, kick off
        // the background worker as soon as the screen blanks. The worker runs
        // on Core 0 so it never blocks NFC or the clock.
        if (wifiConfig.isConnected() && pendingCount > 0 && !isPeakHour()) {
            Serial.println("[Screen] Screen off — triggering immediate background upload");
            lastUploadFlush = now;   // reset the timer so the next fire is a full 10 min later
            if (g_uploadTrigger) xSemaphoreGive(g_uploadTrigger);
        }
    }

    // ── Post-standby timer reset ──────────────────────────────────────────
    // After a long standby all interval timers have huge elapsed values.
    // Reset them to 'now' so they don't all fire simultaneously on the first
    // loop after wake, which would block the clock and NFC for several seconds.
    if (g_clockNeedsReset) {
        // g_clockNeedsReset is also used by the clock section — don't clear it
        // here; let the clock section clear it after anchoring lastClock.
        lastStats       = now;
        lastWifiRetry   = now;
        lastUploadFlush = now;
        lastSocketPoll  = now;
        // lastNFCPoll is intentionally not reset — we want NFC responsive immediately
    }
    bool isConnected = wifiConfig.isConnected();
    if (isConnected && !wasConnectedGlobal) {
        // WiFi reconnected after a drop — re-sync time and re-seed only.
        // Debounced: a marginal signal can make WiFi.status() flap connected/
        // disconnected repeatedly within seconds (each flap re-enters this
        // block), which was re-triggering a full 5-10s seed back-to-back with
        // no gap — the "log never stops" symptom. A genuine reconnect after
        // being offline a while still fires immediately the first time; only
        // rapid re-flapping within the cooldown gets skipped.
        static unsigned long lastReconnectResyncMs = 0;
        wasConnectedGlobal = true;
        if (now - lastReconnectResyncMs >= WIFI_RECONNECT_RESYNC_COOLDOWN_MS) {
            lastReconnectResyncMs = now;
            Serial.println("[WiFi] Reconnected");
            syncNTPTime();
            initialSyncDone = false;   // allow re-seed after reconnect
            triggerInitialSync();
        } else {
            Serial.println("[WiFi] Reconnected (flap within cooldown — skipping re-seed)");
        }
    }
    if (!isConnected && wasConnectedGlobal) {
        wasConnectedGlobal = false;
        Serial.println("[WiFi] Lost — SD-only mode");
        updateStatusDots(false, SDDatabase::isReady(), true);
    }

    // ── WiFi reconnect retry ──────────────────────────────────────────────
    if (!isConnected && (now - lastWifiRetry >= WIFI_RETRY_MS)) {
        lastWifiRetry = now;
        WiFi.reconnect();
    }

    // ── Midnight rollover: detect the date changing ────────────────────────
    // dateStr() flips the instant the clock crosses 00:00:00, so this fires
    // once per day. We don't purge anything here — just mark yesterday's
    // date as "pending cleanup" and (on Mondays) flag a weekly employee
    // re-sync. Both are carried out below, gated on being safe to do so.
    {
        String todayStr = dateStr();
        if (todayStr.length() == 10) {
            if (g_lastSeenDate.length() == 0) {
                g_lastSeenDate = todayStr;   // first time the clock is synced — just anchor, no rollover yet
            } else if (todayStr != g_lastSeenDate) {
                Serial.printf("[Rollover] Date changed %s -> %s\n",
                              g_lastSeenDate.c_str(), todayStr.c_str());
                g_cleanupPendingDate = g_lastSeenDate;  // yesterday — purge once fully synced
                g_lastSeenDate = todayStr;

                time_t t = (time_t)clkEpoch;
                struct tm* tmNow = gmtime(&t);
                if (tmNow->tm_wday == 1) {  // Monday
                    Serial.println("[Rollover] Monday — weekly employee re-sync flagged");
                    g_weeklyRefreshPending = true;
                }
            }
        }
    }

    // ── Midnight rollover: carry out the purge once safe ───────────────────
    // "Safe" = pendingCount reached 0, i.e. everything queued from that date
    // got a successful server response (flushPending() keeps failed uploads
    // in the queue for retry — see its RETRY-later path — so this never
    // fires while something from that date is still unconfirmed). If uploads
    // are still trickling in, this simply re-checks every tick — cheap, just
    // a string + int compare — until the queue drains, then purges once.
    if (g_cleanupPendingDate.length() == 10 && pendingCount == 0) {
        purgeSyncedAttendanceDate(g_cleanupPendingDate);
        g_cleanupPendingDate = "";
    }

    // ── Weekly employee/profile re-sync — runs once the dashboard is idle ──
    // Uses the same force-sync path as the portal's manual "Sync Employees"
    // button, so it inherits the same on-screen progress UI. That's why it
    // waits for STATE_DASHBOARD rather than running silently mid-scan.
    if (g_weeklyRefreshPending && isConnected && currentState == STATE_DASHBOARD) {
        g_weeklyRefreshPending = false;
        Serial.println("[Rollover] Running weekly employee re-sync");
        EmployeeSync::fullSyncIfNeeded(attService, true);
        drawStaticUI();
        updateStatusDots(isConnected, SDDatabase::isReady(), true);
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
            requestSeedToday();   // runs on Core 0 — loop() isn't blocked, stats
                                   // refresh automatically via g_pendingStatsRefresh
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

    // ── Reconcile poller (server-side deletions, portal edits) ─────────────
    // Every reconcilePollRateMs (base 15s) AFTER THE PREVIOUS RUN FINISHED,
    // asks the seed worker to re-run PASS 1-3 so a row deleted on the portal
    // disappears from the device without needing a reboot/WiFi-blip/manual
    // Reseed. Gated on g_seedFinishedAtMs (not dispatch time) and !g_seedRunning
    // because a full pass can take 30-50s+ with verbose per-employee logging —
    // longer than the 15s base interval. Gating on dispatch time let 2-3 poll
    // ticks land mid-run; each queued a semaphore give, so the worker started
    // its next pass with zero gap the instant the current one finished — the
    // "log never stops" symptom. Same idle/peak/heap guards as the socket
    // poller above; the request itself runs on Core 0 (seedWorkerTask) so it
    // never blocks NFC polling on Core 1.
    if (isConnected && currentState == STATE_DASHBOARD &&
        !isPeakHour() && !g_seedRunning &&
        (now - g_seedFinishedAtMs >= reconcilePollRateMs)) {
        if (ESP.getFreeHeap() > 40000) {
            requestSeedToday();

            if (g_lastSeedOk) {
                reconcileErrorCount  = 0;
                reconcilePollRateMs  = RECONCILE_POLL_BASE_MS;
            } else {
                reconcileErrorCount++;
                reconcilePollRateMs = min(
                    (unsigned long)(RECONCILE_POLL_BASE_MS * (1UL << min(reconcileErrorCount, 4))),
                    (unsigned long)RECONCILE_POLL_MAX_MS);
                Serial.printf("[Reconcile] Backed off to %lus (error #%d)\n",
                              reconcilePollRateMs / 1000, reconcileErrorCount);
            }
        }
    }

    // ── Background upload scheduler ───────────────────────────────────────
    // Signals the upload worker task (Core 0) to run flushPending().
    // flushPending() makes HTTP calls (up to 8s) — running it here in loop()
    // would block NFC polling and the clock. The worker task handles it instead.
    if (isConnected && pendingCount > 0 &&
        currentState == STATE_DASHBOARD &&
        (now - lastUploadFlush >= UPLOAD_FLUSH_MS)) {
        lastUploadFlush = now;
        // Wake the upload worker — non-blocking, returns immediately
        if (g_uploadTrigger) xSemaphoreGive(g_uploadTrigger);

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

        case STATE_NFC_LOADING:
            // The worker task (Core 0) does the actual work and calls enterState()
            // when done. loop() just watches here:
            //  • Normal: worker finishes and sets STATE_NFC_PROFILE itself.
            //  • Watchdog: if worker takes > 13s (HTTP stuck despite timeouts),
            //    force back to dashboard so the system is never permanently frozen.
            if (stateElapsed() > 13000) {
                Serial.println("[WDT] STATE_NFC_LOADING exceeded 13s — forcing dashboard");
                if (g_nfcTask) vTaskSuspend(g_nfcTask);   // kill hung worker
                if (g_nfcDone) xSemaphoreGive(g_nfcDone); // reset semaphore
                if (g_nfcTask) vTaskResume(g_nfcTask);     // restart it
                enterState(STATE_DASHBOARD);
                drawStaticUI();
                updateStatusDots(isConnected, SDDatabase::isReady(), true);
                g_pendingStatsRefresh = true;   // defer SD reads to next tick
                resetScreenTimer();
            }
            break;

        case STATE_NFC_PROFILE: {
            // During peak hours use a shorter display window so the queue moves faster.
            unsigned long displayMs = isPeakHour() ? PROFILE_DISPLAY_PEAK_MS : PROFILE_DISPLAY_MS;

            // A buffered next-card also short-circuits the wait immediately:
            // the current person has clearly moved on, so show the next one now.
            bool nextCardReady = (_nextPendingCard.length() > 0);

            if (stateElapsed() >= displayMs || nextCardReady) {
                enterState(STATE_DASHBOARD);
                drawStaticUI();
                updateStatusDots(isConnected, SDDatabase::isReady(), true);
                // FIX: defer SD stat reads to next loop tick so NFC polling
                // resumes immediately. The SD file scan in countTodayCheckIns/
                // countTodayCheckOuts can take 50-200ms and was causing the
                // dashboard to appear frozen / miss rapid back-to-back taps.
                g_pendingStatsRefresh = true;
                if (lastEmployee.hasData) {
                    char timeShort[6];
                    snprintf(timeShort, sizeof(timeShort), "%02d:%02d", clkH, clkM);
                    bool wasIn = (lastClockType.endsWith("_in"));
                    updateLastScan(lastEmployee.fullName,
                                   wasIn ? "check-in" : "check-out",
                                   String(timeShort));
                }
                resetScreenTimer();

                // If a card was buffered while we were showing the profile,
                // process it immediately without waiting for the next loop tick.
                if (nextCardReady) {
                    String buffered = _nextPendingCard;
                    _nextPendingCard = "";
                    Serial.println("[NFC] Processing buffered scan-ahead card: " + buffered);
                    handleNFCDetected(buffered);
                }
            }
            break;
        }

        case STATE_NFC_ERROR:
            if (stateElapsed() >= PROFILE_DISPLAY_MS) {
                enterState(STATE_DASHBOARD);
                drawStaticUI();
                updateStatusDots(isConnected, SDDatabase::isReady(), true);
                // FIX: same deferred refresh — don't block NFC on SD reads here.
                g_pendingStatsRefresh = true;
                resetScreenTimer();   // activity just happened — restart timeout
            }
            break;

        default: break;
    }

    // ── Clock tick ────────────────────────────────────────────────────────
    if (currentState == STATE_DASHBOARD) {
        // After a screen wake, anchor lastClock to now so catchUpClock()
        // starts fresh — the RTC resync already corrected clkH/clkM/clkS.
        if (g_clockNeedsReset) {
            lastClock = millis();
            g_clockNeedsReset = false;
        }
        int ticked = catchUpClock(lastClock);
        if (ticked > 0) {
            tick += (uint8_t)ticked;
            if (!screenIsOff) {                          // skip TFT writes when off
                pulseStatus(tick % 2);
                updateClock(clkH, clkM, clkS);
                if (tick % 60   == 0) updateDate(buildDateStr());
            }
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
    // g_pendingStatsRefresh is set by STATE_NFC_PROFILE/ERROR transitions
    // so the SD scan happens on the NEXT loop() tick, not the same tick
    // as drawStaticUI() — this prevents blocking rapid back-to-back NFC taps.
    if (g_pendingStatsRefresh && currentState == STATE_DASHBOARD && !screenIsOff) {
        g_pendingStatsRefresh = false;
        lastStats = now;   // reset the periodic timer so we don't double-count
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
    }

    if (g_pendingLastScanClear && currentState == STATE_DASHBOARD && !screenIsOff) {
        g_pendingLastScanClear = false;
        clearLastScan("both");
    }

    if (currentState == STATE_DASHBOARD && !screenIsOff &&
        (now - lastStats >= STATS_REFRESH_MS)) {
        lastStats = now;
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
    }

    // ── NFC poll ──────────────────────────────────────────────────────────
    if (currentState != STATE_NFC_LOADING &&
        (now - lastNFCPoll >= NFC_POLL_INTERVAL_MS)) {
        lastNFCPoll = now;

        uint8_t uid[7] = {0}; uint8_t uidLen = 0;
        // 50ms timeout: minimum reliable value for PN532 ISO14443A.
        // The RF field needs ~20ms to energize the card + framing overhead.
        // Going below 50ms causes missed reads on NTAG/MIFARE cards.
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

            } else if (currentState == STATE_NFC_PROFILE || currentState == STATE_NFC_ERROR) {
                // Profile is showing. Buffer this card (if it's a different employee)
                // so we can process it the moment the display clears.
                // Same card as what's currently showing is ignored (still cooling down).
                if (cardId != lastAcceptedCardId) {
                    if (_nextPendingCard != cardId) {
                        _nextPendingCard = cardId;
                        Serial.println("[NFC] Buffered scan-ahead: " + cardId);
                    }
                }

            } else if (currentState != STATE_DASHBOARD) {
                // NFC_LOADING in progress — drop silently

            } else {
                _lastRawCard   = cardId;
                _cardConfirmCt = CARD_CONFIRM_NEEDED;
                _nextPendingCard = "";   // clear any stale buffer
                handleNFCDetected(cardId);
            }

        } else {
            if (_cardConfirmCt > 0) { _lastRawCard = ""; _cardConfirmCt = 0; }
        }
        nfc_poll_end:;
    }
}