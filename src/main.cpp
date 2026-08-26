// ════════════════════════════════════════════════════════════════════════════
// main.cpp — ESP32-S3 WROOM-1  NFC Attendance  (Portrait 240×320)
//
// OPTIMIZATIONS v3:
//
//  1. FAST BOOT       — SD, NFC, WiFi, NTP start immediately. No blocking
//                       delays after each step. Total target boot < 4s.
//
//  2. FAST TAP        — NFC → SD write → profile display in < 200ms.
//                       Profile shown for PROFILE_DISPLAY_MS (1.5s, unified
//                       across peak and non-peak — see define below),
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
#include "screensaver.h"
#include "nfc_manager.h"
#include "WiFiConfig.h"
#include "WiFiManager.h"
#include "pin_assignments.h"
#include "employee_profile_display.h"
#include "attendance_http_service.h"
#include "sd_database.h"
#include "employee_sync.h"
#include "sd_mutex.h"
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
// Idle reconcile burst — fires only while the screen is blanked (nobody is
// actively using the device, so it's a safe moment to pull server-side
// edits/deletes without disrupting a tap). Runs IDLE_RECONCILE_BURST_COUNT
// polls spaced IDLE_RECONCILE_INTERVAL_MS apart, then rests
// IDLE_RECONCILE_REST_MS before starting another burst — so a long idle
// stretch (e.g. overnight) doesn't hammer the device or the server. A tap
// always wins: wakeScreen() resets the burst state immediately, and the
// burst loop itself is gated on screenIsOff so it simply stops firing the
// instant the screen wakes. Also gated on isAdminWorkingHours() (08:00–
// 17:00) — see that function's comment.
//
// IDLE_RECONCILE_INTERVAL_MS was tried at 15s and measured on-device: a
// single seed pass (esp32-sync fetch + CSV reseed + PASS 3 + PASS 3B, each
// doing multi-chunk encrypted HTTP round-trips) routinely took 1-4 minutes
// on its own. Since !g_seedRunning blocks overlap, a 15s interval never
// actually got to space anything out — passes just chained back-to-back
// with heap sitting around 53-59KB free (uncomfortably close to the 40000
// floor) for minutes at a stretch. 2 min is long enough to exceed a single
// pass's duration, so it produces genuine idle time between passes instead
// of continuous back-to-back network/heap churn.
#define IDLE_RECONCILE_INTERVAL_MS   120000     // 2 min between polls within a burst
#define IDLE_RECONCILE_BURST_COUNT   10         // polls per burst (~20 min of polling)
#define IDLE_RECONCILE_REST_MS       1800000    // 30 min rest between bursts

// Admin working hours — the portal is only actively edited/updated during
// this window, so outside of it there's nothing new for the idle burst to
// pull. Polling past 17:00 or before 08:00 would just be waking up WiFi/SD
// for no reason until the admin is back at their desk.
#define ADMIN_HOURS_START_MIN   (8 * 60)    // 08:00
#define ADMIN_HOURS_END_MIN     (17 * 60)   // 17:00

#define RECONCILE_POLL_BASE_MS   1800000    // 30 min — was 15s; 15s was hammering the ESP32's
                                             // heap with HTTP/JSON churn and freezing the UI
                                             // during busy tap lines (morning/noon/afternoon out)
#define RECONCILE_POLL_MAX_MS    1800000    // cap backoff at 30 min too (was 2 min, now irrelevant
                                             // since base == max, but kept for the min()/backoff math)
#define SCREEN_TIMEOUT_MS     300000   // 5 minutes of inactivity → show screensaver

// ── PROFILE DISPLAY ───────────────────────────────────────────────────────────
// How long the employee photo/badge stays on screen after a scan.
// Normal:   2000ms — comfortable read time during quiet periods.
// Peak:      800ms — fast turnover during rush-hour lineups.
// The display auto-shortens during peak hours so the queue moves faster.
// A new card from a DIFFERENT employee also short-circuits the wait immediately.
// Unified display idle-timeout, day-round. Previously peak hour used a
// separate, shorter 800ms window to keep the line moving — but with the
// instant-swap behavior in STATE_NFC_PROFILE (a buffered next tap always
// replaces the current profile right away, regardless of this timer), the
// timer only ever matters for the gap AFTER the last tap in a burst. 1.5s
// there is a middle ground: noticeably faster than the old 3s idle window,
// without cutting it so close that someone glancing at their own profile
// misses seeing it.
#define PROFILE_DISPLAY_MS      1500
// Error/warning screens (e.g. "Tap too quick") get more time than a normal
// success profile — 2s wasn't enough to actually read a message that's new
// to the employee, since the old, small, un-split text was hard to read in
// that window too. 3s gives enough time to read it now that it's rendered
// bigger and on two clear lines (see EmployeeProfileDisplay::showError()).
#define NFC_ERROR_DISPLAY_MS    3000

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
// Employee IDs on these tags are fixed-format numeric strings (e.g.
// "2404141283567", 13 digits). MIN_CARD_ID_LEN=8 used to let obviously-
// truncated reads like "240414128" (9 digits) or "24041412835" (11 digits)
// through to a full SD lookup / HTTP auth call that was guaranteed to fail
// (wrong string, safely denied) — costing a wasted round trip and forcing a
// re-tap. Raised to 12 so a read has to be nearly complete before it's even
// considered a candidate; combined with isAllDigits() below (rejects reads
// with a glitched byte turning a digit into punctuation, e.g. the
// '2400!41283567' case in nfc_manager.cpp), this filters out bad reads
// locally in under a millisecond instead of round-tripping to find out.
#define MIN_CARD_ID_LEN         12
// Fallback threshold for a SHORT read (card left the RF field before the
// full ID was captured — see nfc_manager.cpp / SDDatabase::loadUidForNfcPrefix).
// A read this long or longer, but still short of MIN_CARD_ID_LEN, is allowed
// to try a *local* prefix lookup against the cached employee list instead of
// being rejected outright. loadUidForNfcPrefix() only ever returns a hit when
// exactly one cached employee's ID starts with the read prefix, so this is
// safe as long as MIN_PREFIX_LEN digits is actually enough to be unique
// across your roster — verify with:
//   cut -c1-6 id_numbers.txt | sort | uniq -c | sort -rn | head
// and raise this value if any prefix at that length is shared by 2+ people.
#define MIN_PREFIX_LEN           6
#define SCAN_COOLDOWN_MS       3500    // same-card lockout
// Raised 1 -> 2: with the poll cadence at NFC_POLL_INTERVAL_MS (30ms), this
// costs ~30-60ms of extra wait on a card that's actually being held on the
// reader (negligible to a person's tap), but means a single-poll glitch can
// no longer be acted on immediately — the very next poll has to agree
// first. This is a distinct safety net from the nfc_manager.cpp page-level
// consensus check: that one guards against a bad single page transaction,
// this one guards against a bad read of the whole tag (e.g. mid-lift-off).
#define CARD_CONFIRM_NEEDED      2

#define AP_SSID     "JJC_Attendance_Config"
#define AP_PASSWORD "ilovejjcenggworks"
#define SERVER_URL  "https://jjcenggworks.com"

String deviceId = "Attendance_Display_01";

// ─── Software clock ──────────────────────────────────────────────────────────
// volatile + critical section: clkH/clkM/clkS/clkEpoch are written by
// tickClock() on Core 1 (loop()'s free-running per-second ticker) AND by
// resyncClockFromRTC() on Core 0 (called from wakeScreen(), which the NFC
// task calls on every tap). Without synchronization, a tap's RTC resync
// (Core 0) and a tick (Core 1) landing at the same moment can interleave
// their writes to these 4 fields — e.g. Core 1's tick can silently overwrite
// a freshly RTC-corrected clkEpoch with a stale pre-resync value +1 second,
// leaving clkH/clkM/clkS showing the correct new time but clkEpoch (which
// dateStr() derives the logged date from) still one day behind. That's what
// produced attendance records with the right time but the previous day's
// date. portMUX_TYPE serializes the writers so a tick and a resync can never
// interleave field-by-field.
static portMUX_TYPE g_clockMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  clkH = 0, clkM = 0, clkS = 0;
static volatile uint32_t clkEpoch = 0;

static void tickClock() {
    portENTER_CRITICAL(&g_clockMux);
    // Rewritten as explicit read-modify-write (x = x + 1) instead of ++x
    // on a volatile: pre/post-increment of volatile-qualified values is
    // deprecated in C++20 and was producing -Wvolatile warnings. Behavior
    // is unchanged, just spelled out instead of relying on ++.
    clkS = clkS + 1;
    if (clkS >= 60) { clkS = 0;
        clkM = clkM + 1;
        if (clkM >= 60) { clkM = 0;
            clkH = clkH + 1;
            if (clkH >= 24) { clkH = 0; if (clkEpoch) clkEpoch += 86400; }
        }
    }
    if (clkEpoch) clkEpoch = clkEpoch + 1;
    portEXIT_CRITICAL(&g_clockMux);
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
// clkEpoch == 0 means the software clock has never been NTP/RTC-synced —
// clkH/clkM/clkS are just free-running ticks from 00:00:00 since boot
// (the "stopwatch" symptom). Callers should show a placeholder instead
// of painting those raw values as if they were real time.
static inline bool clockIsSynced() { return clkEpoch != 0; }
// Defined below, near ensureClockSyncedOrReboot() — forward-declared here
// because wakeScreen() (which needs it) comes first in the file.
static bool handleUnsyncedClockOrReboot(bool onScreensaver);

static String clockStr() {
    char b[12]; snprintf(b, sizeof(b), "%02d:%02d:%02d", clkH, clkM, clkS);
    return b;
}
static String dateStr() {
    if (!clkEpoch) return "";
    time_t t = (time_t)clkEpoch;
    // clkEpoch is a true UTC instant (mktime() below converts the local
    // wall-clock reading to UTC before storing it). Must decode it back
    // with localtime_r(), NOT gmtime() — gmtime() reads the UTC calendar
    // date, which for a UTC+8 site is still "yesterday" from local
    // midnight until 08:00 local time. That mismatch was silently
    // stamping every clock-in before 8 AM with the previous day's date
    // even though the displayed/logged HH:MM:SS was correct.
    struct tm tm;
    localtime_r(&t, &tm);
    char b[12];
    snprintf(b, sizeof(b), "%04d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return b;
}
static String buildDateStr() {
    if (clkEpoch) {
        static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        time_t t = (time_t)clkEpoch;
        // See dateStr() above — must use localtime_r(), not gmtime().
        struct tm tm;
        localtime_r(&t, &tm);
        char b[32];
        snprintf(b, sizeof(b), "%s - %04d-%02d-%02d",
                 days[tm.tm_wday],
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return String(b);
    }
    return "--- AWAITING SYNC ---";
}

// ── RUSH-MODE CONFIGURATION ───────────────────────────────────────────────────
// Long employee lines form at the "major" clock-out taps — 2 of the 3
// session outs from resolveClockType()'s SESSIONS (morning_out/noon,
// afternoon_out/5pm) happen at a fixed, known time; evening_out is
// overtime and has no fixed schedule, so it isn't listed here — add a
// third { center, ... } row below if your site adopts a fixed OT cutoff.
//
// One table drives BOTH behaviors so the numbers can't drift apart:
//   blockLeadMin — minutes before `center` background HTTP work (STEP-3
//                  cache-miss server auth, idle-reconcile seed polling)
//                  is shut off. isPeakHour() uses this.
//   wakeLeadMin  — minutes before `center` the screen is FORCE-WOKEN (even
//                  if nobody has tapped yet) and held awake, so the first
//                  person in line finds an already-on reader instead of
//                  spending their tap waking it from the screensaver.
//                  Always <= blockLeadMin, so the wake window sits inside
//                  the background-block window. isRushPrewakeActive() uses
//                  this.
//   tailMin      — minutes AFTER `center` both windows stay open, to catch
//                  the tail of the line.
struct RushWindow { uint16_t center; uint16_t blockLeadMin; uint16_t wakeLeadMin; uint16_t tailMin; };
static const RushWindow RUSH_WINDOWS[] = {
    { 12*60,  10, 5, 10 },   // noon out       — block 11:50, wake 11:55, end 12:10
    { 17*60,  10, 5, 15 },   // afternoon out  — block 16:50, wake 16:55, end 17:15
};

// ── PEAK HOUR DETECTION ───────────────────────────────────────────────────────
// Returns true if clock is currently inside a peak upload-defer window OR
// within PEAK_DEFER_MIN minutes after one ended. Windows come from
// RUSH_WINDOWS above (blockLeadMin/tailMin) — see that table's comment for
// why the lead-in is as wide as it is.
static uint16_t peakEndMin = 0xFFFF;  // 0xFFFF = no peak seen yet

static bool isPeakHour() {
    uint16_t nowMin = (uint16_t)clkH * 60 + clkM;

    for (auto& w : RUSH_WINDOWS) {
        uint16_t s = w.center - w.blockLeadMin;
        uint16_t e = w.center + w.tailMin;
        if (nowMin >= s && nowMin <= e) {
            peakEndMin = e;     // update so post-peak hold works
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

// isRushPrewakeActive — true from wakeLeadMin minutes before a major
// clock-out through tailMin minutes after it. Always a subset of (or equal
// to) isPeakHour()'s window for the same entry, since wakeLeadMin <=
// blockLeadMin. Used by loop() to force-wake the screen ahead of the line
// and to suspend the normal screen/screensaver timeout for the duration,
// so the reader is already lit and ready for every tap in the rush instead
// of the first person's tap having to wake it.
static bool isRushPrewakeActive() {
    uint16_t nowMin = (uint16_t)clkH * 60 + clkM;
    for (auto& w : RUSH_WINDOWS) {
        uint16_t s = w.center - w.wakeLeadMin;
        uint16_t e = w.center + w.tailMin;
        if (nowMin >= s && nowMin <= e) return true;
    }
    return false;
}

// isAdminWorkingHours — true only within the admin's 08:00–17:00 window.
// The idle reconcile burst is gated on this: outside working hours nothing
// on the server is being edited, so there's nothing new to pull, and
// polling would just be waking WiFi/SD for no reason until the admin is
// back. NOT used by isPeakHour()/uploads — those stay on their own
// always-on schedule regardless of admin hours.
static bool isAdminWorkingHours() {
    uint16_t nowMin = (uint16_t)clkH * 60 + clkM;
    return nowMin >= ADMIN_HOURS_START_MIN && nowMin <= ADMIN_HOURS_END_MIN;
}

// ─── Globals ─────────────────────────────────────────────────────────────────
WiFiConfig             wifiConfig(AP_SSID, AP_PASSWORD);
WiFiManager            wifiManager;
AttendanceHTTPService  attService(SERVER_URL);
EmployeeProfileDisplay* empDisplay = nullptr;

static bool initialSyncDone = false;

// ─── Screen timeout ───────────────────────────────────────────────────────────
// volatile: read/written from both loop() (Core 1) and nfcWorkerTask (Core 0).
// Without volatile the compiler can cache stale copies per-core, so a tap
// landing on Core 0 could read a stale screenIsOff==false and skip
// wakeScreen() entirely — screen stays dim even though a card was tapped.
static volatile unsigned long lastActivityMs  = 0;   // last time the screen was "touched"
static volatile bool          screenIsOff     = false;

// ─── Screensaver burn-in protection ────────────────────────────────────────────
// A second, shorter timer that fires only while the screensaver is already
// showing: after DIM_AFTER_SCREENSAVER_MS at full brightness, fade the
// backlight down so a static logo/clock doesn't sit at full brightness
// indefinitely (LCD panels can still develop image persistence, just more
// slowly than OLED). Restored to full brightness by wakeScreen().
#define DIM_AFTER_SCREENSAVER_MS   10000   // 10 seconds into the screensaver
// Previously 60 (~24% duty) — on this backlight LED, PWM brightness doesn't
// scale linearly with duty cycle, so 24% still looked almost full-bright and
// read as "not dimming" even though the fade was running correctly. Dropped
// to a much darker level that's still non-zero (never fully black, so the
// clock/date stay legible) but is now unmistakably dimmer than the initial
// screensaver brightness.
#define SCREENSAVER_DIM_LEVEL      18      // ~7% brightness — clearly dim, still readable
// volatile: g_screensaverDimmed is cleared by wakeScreen() on Core 0 (NFC
// task) but set by loop() on Core 1 — same cross-core visibility issue as
// screenIsOff above.
static volatile unsigned long g_screensaverEnteredMs = 0;
static volatile bool          g_screensaverDimmed    = false;

static bool          wasConnectedGlobal = false;  // tracks WiFi state across loop()

// Set true by wakeScreen() so loop()'s clock branch resets lastClock to now,
// preventing a huge catch-up burst of ticks after a long standby.
// volatile: set on Core 0 (nfcWorkerTask -> wakeScreen()), read/cleared on
// Core 1 (loop()) — same cross-core visibility issue as the flags above.
static volatile bool g_clockNeedsReset = false;

// Re-sync the software clock from the ESP32 RTC (no network needed).
// The RTC keeps ticking during standby / screen-off, so this instantly
// corrects any drift that accumulated while the display was blanked.
static void resyncClockFromRTC() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        // mktime() takes a libc mutex internally (via tzset), which is not
        // safe to call inside a FreeRTOS critical section - compute it
        // before entering the critical section (see syncNTPTime() for the
        // same fix and full explanation).
        uint32_t epoch = (uint32_t)mktime(&timeinfo);
        portENTER_CRITICAL(&g_clockMux);
        clkH     = timeinfo.tm_hour;
        clkM     = timeinfo.tm_min;
        clkS     = timeinfo.tm_sec;
        clkEpoch = epoch;
        portEXIT_CRITICAL(&g_clockMux);
        Serial.printf("[Clock] RTC resync on wake: %02d:%02d:%02d\n",
                      clkH, clkM, clkS);
    } else {
        // RTC unavailable (no NTP yet) — keep rolling from wherever we are
        Serial.println("[Clock] RTC unavailable on wake, keeping software clock");
    }
}

// Idle reconcile burst state — declared up here (file scope, not inside
// loop()) so both wakeScreen() and loop() can see/reset it. A tap must be
// able to cancel an in-progress idle burst immediately, which means the
// state can't be local static to loop() alone.
static int           g_idlePollCount       = 0;
static bool          g_idleResting         = false;
static unsigned long g_idleRestStartedAtMs = 0;
static unsigned long g_lastIdlePollMs      = 0;

static void wakeScreen() {
    if (screenIsOff) {
        screenIsOff = false;
        g_screensaverDimmed = false;   // next screensaver entry starts at full brightness

        // Resync software clock from RTC so the display jumps to the correct
        // time immediately rather than catching up second-by-second.
        resyncClockFromRTC();
        g_clockNeedsReset = true;   // anchor lastClock to now in loop()

        TFTDisplayManager::backlightOn();
        drawStaticUI();
        if (clockIsSynced()) updateClock(clkH, clkM, clkS);
        else                 handleUnsyncedClockOrReboot(false);
        updateDate(buildDateStr());
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
        Serial.println("[Screen] Wake — screen restored");

        // A tap always wins — cancel any idle burst in progress so the next
        // screen-off period starts a fresh burst of 10 rather than resuming
        // mid-burst or mid-rest.
        g_idlePollCount       = 0;
        g_idleResting         = false;
        g_idleRestStartedAtMs = 0;
    }
    lastActivityMs = millis();
}
static void resetScreenTimer() { lastActivityMs = millis(); }

// Flags settable from web portal actions (checked in loop)
volatile bool g_triggerEmployeeSync = false;   // download employees from server
volatile bool g_triggerPhotoSync    = false;   // download missing photos
volatile bool g_triggerReseedToday  = false;   // fetch today's attendance from server
volatile bool g_triggerScreenWake   = false;   // portal "Wake Screen" button
volatile bool g_triggerScreenSleep  = false;   // portal "Sleep Screen" button

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

// Employee IDs on these tags are pure numeric strings (see MIN_CARD_ID_LEN
// comment above). A single flipped bit during a noisy RF read can turn a
// digit into punctuation (the '2400!41283567' case in nfc_manager.cpp's
// comments) without changing the length — so the length check alone
// doesn't catch every bad read. This is a cheap O(n) scan, safe to run on
// every poll.
static bool isAllDigits(const String& s) {
    if (s.length() == 0) return false;
    for (unsigned int i = 0; i < s.length(); i++) {
        if (!isDigit(s.charAt(i))) return false;
    }
    return true;
}

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

    // BUG FIX: an earlier session left open (e.g. "morning_in" logged but no
    // "morning_out") used to get silently skipped once the clock rolled into
    // the next session's window — the old logic only ever looked at hasIn/
    // hasOut for `cur`, saw the current session had no "_in" yet, and handed
    // back "<cur>_in" (e.g. "afternoon_in") even though the employee still
    // had an open morning session. That's exactly the SERVER_SEED case: a
    // morning_in exists with no morning_out, and the very next tap — already
    // past noon — jumped straight to afternoon_in instead of closing morning
    // out. Now we check every earlier session first and close the oldest
    // still-open one before ever considering starting the current session.
    for (int si = 0; si < cur; si++) {
        if (hasIn[si] && !hasOut[si]) return String(SESSIONS[si]) + "_out";
    }

    if (hasIn[cur] && !hasOut[cur]) return String(SESSIONS[cur]) + "_out";
    if (!hasIn[cur])                 return String(SESSIONS[cur]) + "_in";

    // Current session already fully clocked (duplicate tap) — fall back
    // to the first genuinely open slot elsewhere in the day (this also
    // covers a LATER session left open by a manual edit).
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

    // Same fix as above, applied to the non-SDDB_HAS_LOAD_TODAY_CLOCK_TYPES
    // build: close out any earlier still-open session before opening `cur`.
    for (int si = 0; si < cur; si++) {
        if (hasIn[si] && !hasOut[si]) return String(SESSIONS[si]) + "_out";
    }

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

// Sized generously so the in-RAM/SD-mirrored sync queue can absorb a rush
// of taps (e.g. a busy clock-out window) without silently dropping records
// while uploads are paused by isPeakHour().
#define PENDING_QUEUE_CAP 200

// Safety net: a record that fails this many times in a row (whether via
// flushPending()'s single-record path or the bulk upload path) is
// quarantined (dropped from the active queue + logged to
// quarantined_records.log for manual review) instead of retried forever.
// File-scope (not local to one function) so both paths share one cap.
#define MAX_RETRIES_BEFORE_QUARANTINE 20

struct PendingRecord {
    String empUid, nfcUid, clockType, timestamp, date;
    int retryCount = 0;   // consecutive upload failures — see MAX_RETRIES_BEFORE_QUARANTINE
};
static PendingRecord pendingQueue[PENDING_QUEUE_CAP];
static int           pendingCount = 0;

// Overwrites PENDING_QUEUE_PATH with the current in-memory queue.
// Called after every change so the SD copy never falls behind. Up to
// PENDING_QUEUE_CAP short records is still a small write — fine to do inline.
static void savePendingQueueToSD() {
    if (!SDDatabase::isReady()) return;
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks — see sd_mutex.h

    DynamicJsonDocument doc(PENDING_QUEUE_CAP * 160);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < pendingCount; i++) {
        JsonObject o = arr.createNestedObject();
        o["empUid"]    = pendingQueue[i].empUid;
        o["nfcUid"]    = pendingQueue[i].nfcUid;
        o["clockType"] = pendingQueue[i].clockType;
        o["timestamp"] = pendingQueue[i].timestamp;
        o["date"]      = pendingQueue[i].date;
        o["retryCount"] = pendingQueue[i].retryCount;
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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks — see sd_mutex.h
    if (!SD_MMC.exists(PENDING_QUEUE_PATH)) return;

    File f = SD_MMC.open(PENDING_QUEUE_PATH, FILE_READ);
    if (!f) return;

    DynamicJsonDocument doc(PENDING_QUEUE_CAP * 160);
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
        if (pendingCount >= PENDING_QUEUE_CAP) break;
        pendingQueue[pendingCount++] = {
            String((const char*)(o["empUid"]    | "")),
            String((const char*)(o["nfcUid"]    | "")),
            String((const char*)(o["clockType"] | "")),
            String((const char*)(o["timestamp"] | "")),
            String((const char*)(o["date"]      | "")),
            (int)(o["retryCount"] | 0)   // absent in files saved before this field existed — defaults to 0
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
    if (pendingCount < PENDING_QUEUE_CAP) {
        pendingQueue[pendingCount++] = {empUid, nfcUid, clockType, ts, dt};
        Serial.printf("[Queue] +1 pending (%d/%d): %s %s\n",
                      pendingCount, PENDING_QUEUE_CAP, empUid.c_str(), clockType.c_str());
    } else {
        // Should only be reachable if PENDING_QUEUE_CAP is ever exceeded by a
        // single day's rush — logged loudly since a drop here means a tap
        // never reaches the server until someone notices and re-enters it.
        Serial.println("[Queue] FULL — dropping oldest (raise PENDING_QUEUE_CAP)");
        for (int i = 0; i < PENDING_QUEUE_CAP - 1; i++) pendingQueue[i] = pendingQueue[i+1];
        pendingQueue[PENDING_QUEUE_CAP - 1] = {empUid, nfcUid, clockType, ts, dt};
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

    // MAX_RETRIES_BEFORE_QUARANTINE (file scope, near PendingRecord above):
    // without this, ANY server response that recordAttendance() doesn't
    // recognize as a permanent failure — not just the 409 case already
    // handled — would sit at the head of the queue with UPLOAD_BATCH_SIZE=1,
    // retried every ~200-350ms indefinitely, hammering the server and
    // starving every other pending record behind it. 20 tries at the
    // ~200ms loop cadence is well past "give the network a moment," so
    // anything still failing after that is treated as needing a human,
    // not another retry.

    int remaining = 0;
    // static, not stack-local: at PENDING_QUEUE_CAP=200 * 5 Strings/record
    // this is too big for the uploadWorker task's 12KB stack. flushPending()
    // only ever runs on that one task, so reusing a static buffer is safe.
    static PendingRecord keep[PENDING_QUEUE_CAP];

    for (int i = 0; i < pendingCount; i++) {
        if (i < toSend) {
            bool ok = attService.recordAttendance(
                pendingQueue[i].empUid, pendingQueue[i].nfcUid,
                deviceId, pendingQueue[i].clockType,
                pendingQueue[i].timestamp, pendingQueue[i].date);

            if (!ok) {
                pendingQueue[i].retryCount++;
                if (pendingQueue[i].retryCount >= MAX_RETRIES_BEFORE_QUARANTINE) {
                    Serial.printf("[Flush] ⚠ QUARANTINED after %d failed attempts: %s %s "
                                  "— dropped from active queue, see quarantined_records.log\n",
                                  pendingQueue[i].retryCount,
                                  pendingQueue[i].empUid.c_str(),
                                  pendingQueue[i].clockType.c_str());
                    SDLockGuard _sdLock;   // see sd_mutex.h
                    File qf = SD_MMC.open("/logs/quarantined_records.log", FILE_APPEND);
                    if (qf) {
                        char nowStr[24];
                        snprintf(nowStr, sizeof(nowStr), "%s %02d:%02d:%02d",
                                 dateStr().c_str(), clkH, clkM, clkS);
                        qf.printf("[%s] empUid=%s clockType=%s timestamp=%s date=%s "
                                  "retries=%d\n",
                                  nowStr,
                                  pendingQueue[i].empUid.c_str(),
                                  pendingQueue[i].clockType.c_str(),
                                  pendingQueue[i].timestamp.c_str(),
                                  pendingQueue[i].date.c_str(),
                                  pendingQueue[i].retryCount);
                        qf.close();
                    }
                    // not added to keep[] — intentionally dropped from the retry queue
                } else {
                    keep[remaining++] = pendingQueue[i];
                    Serial.printf("[Flush] RETRY later (%d/%d): %s %s\n",
                                  pendingQueue[i].retryCount, MAX_RETRIES_BEFORE_QUARANTINE,
                                  pendingQueue[i].empUid.c_str(),
                                  pendingQueue[i].clockType.c_str());
                }
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
        // Screensaver-corruption guard (see the sync-trigger blocks in loop()
        // for the full explanation): only repaint the status row if the
        // dashboard — not the screensaver — currently owns the display.
        if (currentState == STATE_DASHBOARD && !screenIsOff) {
            updateStatusDots(wifiConfig.isConnected(), SDDatabase::isReady(), true);
        }
    }
}

// ─── NTP Time Sync ────────────────────────────────────────────────────────────
// Survives ESP.restart() (RTC memory keeps its value across a software
// reset) but resets to 0 on a real power cycle — exactly what we want for
// capping consecutive "reboot to retry NTP" attempts without risking an
// infinite bootloop if NTP is unreachable for a longer stretch (bad DNS,
// blocked UDP 123, etc.) instead of just a one-off hiccup.
RTC_DATA_ATTR static uint8_t g_ntpBootRetryCount = 0;
static const uint8_t NTP_BOOT_MAX_REBOOTS = 3;

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
        // mktime() internally takes a libc mutex (via tzset) to read the TZ
        // env var, which is illegal inside a FreeRTOS critical section and
        // was causing abort()/reboot. Compute it beforehand and only guard
        // the shared clock variable assignment with the critical section.
        uint32_t epoch = (uint32_t)mktime(&timeinfo);
        portENTER_CRITICAL(&g_clockMux);
        clkH     = timeinfo.tm_hour;
        clkM     = timeinfo.tm_min;
        clkS     = timeinfo.tm_sec;
        clkEpoch = epoch;
        portEXIT_CRITICAL(&g_clockMux);
        SDDatabase::setDateProvider([]() -> String { return dateStr(); });
        g_ntpBootRetryCount = 0;   // clean sync — reset the reboot-cap streak
        Serial.printf("[Time] OK: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
                      clkH, clkM, clkS);
    } else {
        Serial.println("[Time] NTP failed after 10s");
    }
}

// Boot-time-only guard against the "stopwatch clock" symptom: if WiFi came
// up but NTP never landed, clkEpoch stays 0 and the free-running ++clkS
// ticker in updateClockTick() (below) just counts up from 00:00:00 like a
// stopwatch instead of showing a real time — because as far as it knows,
// 00:00:00 IS the current time. A couple of quick in-place retries usually
// catches a transient NTP hiccup; if those still fail, a fresh reboot (full
// WiFi/NTP re-init) fixes it far more often than retrying in the same
// session does, so we do that — but only up to NTP_BOOT_MAX_REBOOTS in a
// row, so a genuinely unreachable NTP server degrades to "dashboard runs
// with an unsynced clock" instead of bootlooping forever.
void ensureClockSyncedOrReboot() {
    if (!wifiConfig.isConnected()) {
        // No WiFi at boot — expected/acceptable offline mode, not the bug
        // being guarded against here. Don't burn a reboot attempt on it.
        return;
    }
    if (clkEpoch != 0) {
        g_ntpBootRetryCount = 0;   // clean sync — reset the streak for next time
        return;
    }

    Serial.println("[Time] NTP didn't land on first try — retrying before giving up...");
    for (int i = 0; i < 2 && clkEpoch == 0; i++) {
        delay(1000);
        syncNTPTime();
    }
    if (clkEpoch != 0) {
        g_ntpBootRetryCount = 0;
        return;
    }

    if (g_ntpBootRetryCount < NTP_BOOT_MAX_REBOOTS) {
        g_ntpBootRetryCount++;
        Serial.printf("[Time] Still no NTP after retries — rebooting to refresh "
                      "(attempt %d/%d)\n", g_ntpBootRetryCount, NTP_BOOT_MAX_REBOOTS);
        delay(300);
        ESP.restart();
        // unreachable — ESP.restart() doesn't return
    }

    Serial.println("[Time] NTP still unavailable after max reboot attempts — "
                    "continuing with an unsynced clock rather than bootlooping forever");
    g_ntpBootRetryCount = 0;   // don't carry the streak into the next real boot
}

// Runtime counterpart to ensureClockSyncedOrReboot(): called from the
// dashboard/screensaver render paths at the moment they'd otherwise have
// to paint a "--:--:--" stopwatch placeholder because clkEpoch is still 0.
// Per request, that placeholder is now a last resort — the device reboots
// to force a fresh WiFi/NTP re-init first, since a reboot recovers from a
// stuck/never-associated WiFi state that WiFi.reconnect() alone sometimes
// doesn't. Reuses the same RTC_DATA_ATTR g_ntpBootRetryCount/
// NTP_BOOT_MAX_REBOOTS cap as the boot-time guard so a genuinely
// unreachable network (e.g. no WiFi configured at all) degrades to the
// dashed placeholder after a few tries instead of bootlooping forever.
// Returns true if it drew the placeholder (caller should do nothing else
// this tick); false means a reboot was just issued (unreachable in
// practice — ESP.restart() doesn't return).
static bool handleUnsyncedClockOrReboot(bool onScreensaver) {
    if (g_ntpBootRetryCount < NTP_BOOT_MAX_REBOOTS) {
        g_ntpBootRetryCount++;
        Serial.printf("[Time] Clock still unsynced on dashboard — rebooting to "
                      "recover NTP (attempt %d/%d)\n",
                      g_ntpBootRetryCount, NTP_BOOT_MAX_REBOOTS);
        delay(300);
        ESP.restart();
        // unreachable — ESP.restart() doesn't return
    }
    Serial.println("[Time] Clock still unsynced after max reboot attempts — "
                    "showing placeholder rather than bootlooping forever");
    if (onScreensaver) updateScreensaverClockUnsynced();
    else                updateClockUnsynced();
    return true;
}

// ─── Photo cache helper ───────────────────────────────────────────────────────
// NOTE: Photo downloads only happen here as a fallback when a card is scanned
// and no photo is on SD yet. The primary way to populate photos is via the
// web portal "Sync Photos" action (manual, no auto-download at boot).
static String getPhotoPath(const EmployeeProfile& emp) {
    if (!SDDatabase::isReady() || emp.uid.length() == 0) return "";
    if (SDDatabase::hasPhoto(emp.uid)) {
        String cand = SDDatabase::photoPath(emp.uid);
        SDLockGuard _sdLock;   // see sd_mutex.h
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

    // Suspend SDLogger's per-line SD_MMC file write AND Serial.println for the
    // duration of the three passes below — see SDLogger::suspendSDWrite() for
    // why. The CSV/reconcile results themselves are unaffected (those are
    // separate SD_MMC calls, not routed through the logger); this only skips
    // the noisy per-employee DEBUG/INFO trace lines on both outputs. Always
    // resumed via the RAII-style guard so an early return (e.g. PASS 1
    // issues) can't leave it stuck suspended.
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
    SDLockGuard _sdLock;   // see sd_mutex.h

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

// ── Last-seen-date persistence ──────────────────────────────────────────────
// g_lastSeenDate used to be RAM-only, so a reboot around midnight (e.g. the
// SYSTEM PANIC seen in /logs/last_crash.log) meant the device never
// "witnessed" the date rollover live and the previous day's CSV/map/snapshot
// were orphaned on the SD card forever — nothing else ever re-checks them.
// We now mirror the last-seen date to a 10-byte SD file on every rollover,
// and on boot compare the saved value against today: if it's stale, treat
// it exactly like a missed rollover and queue that date for purge once the
// queue is confirmed empty, same as the live-detection path.
#define LAST_SEEN_DATE_PATH "/attendance/last_seen_date.txt"

static void saveLastSeenDateToSD(const String& d) {
    SDLockGuard _sdLock;   // see sd_mutex.h
    File f = SD_MMC.open(LAST_SEEN_DATE_PATH, FILE_WRITE);
    if (!f) return;
    f.print(d);
    f.close();
}

static String loadLastSeenDateFromSD() {
    SDLockGuard _sdLock;   // see sd_mutex.h
    if (!SD_MMC.exists(LAST_SEEN_DATE_PATH)) return "";
    File f = SD_MMC.open(LAST_SEEN_DATE_PATH, FILE_READ);
    if (!f) return "";
    String d = f.readString();
    f.close();
    d.trim();
    return (d.length() == 10) ? d : "";
}

// ── Seed worker — runs seedTodayAttendanceFromServer() on Core 0 ──────────────
// seedTodayAttendanceFromServer() does 2-3 paginated HTTP calls and was
// measured taking 5-10s against ~43 employees. Called directly from loop()
// (Core 1) it used to freeze NFC polling and the clock for that whole
// window — exactly when a WiFi hiccup-then-reconnect during a busy rush
// would trigger it. This task moves it off Core 1, same pattern as
// uploadWorkerTask. Skipped entirely during peak hour, matching every
// other background job in this codebase (upload flush, photo sync, etc).
// ── Cross-task network mutex ────────────────────────────────────────────────
// Upload worker and seed worker are separate FreeRTOS tasks on Core 0, each
// woken independently by its own binary semaphore — nothing previously
// stopped both from being active at the same moment (e.g. a queued upload
// firing right as a WiFi-reconnect reseed kicks off). Neither corrupts the
// other's data (each uses its own local HTTPClient), but running two
// heap-heavy, WiFi-heavy HTTP passes at once on an already memory-tight
// device is exactly the kind of overlap we want to avoid. This mutex makes
// them mutually exclusive: whichever wins the race runs to completion while
// the other simply waits (blocked, not spin-polling) instead of proceeding
// in parallel. NFC's own server-fallback call is intentionally NOT gated by
// this — that path runs while a person is standing at the reader, and making
// it wait behind a 5-10s seed pass would recreate the exact freezing this
// whole task-based design was built to avoid.
static SemaphoreHandle_t g_networkMutex = nullptr;

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
            // Wait for the upload worker to finish if it's mid-flush, then
            // hold the mutex for the whole seed pass so upload can't start
            // underneath us either. portMAX_DELAY here is safe — the upload
            // worker always releases (it never blocks forever on its own).
            if (g_networkMutex) xSemaphoreTake(g_networkMutex, portMAX_DELAY);
            g_lastSeedOk      = seedTodayAttendanceFromServer();
            if (g_networkMutex) xSemaphoreGive(g_networkMutex);
            g_seedFinishedAtMs = millis();
            g_seedRunning     = false;
            // Stack headroom check — logs a warning if this task's worst-case
            // remaining stack drops low, so a near-overflow shows up in the
            // log BEFORE it becomes a silent PANIC/TASK_WDT reset like the
            // one in last_crash.log, instead of after.
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
            if (hwm < 512) {
                Serial.printf("[Seed] ⚠ low stack headroom: %u bytes free (task stack may be too small)\n",
                              (unsigned)(hwm * sizeof(StackType_t)));
            }
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

// ── flushPendingBulk ─────────────────────────────────────────────────────────
// Sends the ENTIRE pending queue in ONE HTTP POST via
// AttendanceHTTPService::postBulkAttendance(), instead of flushPending()'s
// one-record-per-call loop. Cuts N HTTP round trips (TLS handshake + headers
// each) down to 1 — the win that matters most on a slow/high-latency link,
// where per-record overhead dominates actual upload time.
//
// Server-side this batch is delegated to the existing syncAttendanceRecords()
// (see bulk-record-endpoint.php), so it also gets that function's existing
// duplicate handling and post-commit email notifications for free — nothing
// extra needed here for either.
//
// Same retry/quarantine contract as flushPending(): a record only leaves
// pendingQueue[] when it does NOT show up in the response's errors[] list
// (i.e. it was either inserted or recognized as a duplicate — either way
// the server already has it, so retrying is pointless, same reasoning as
// recordAttendance()'s 409 handling). Anything listed in errors[] stays
// queued and its retryCount increments, hitting MAX_RETRIES_BEFORE_QUARANTINE
// the same as the single-record path.
static void flushPendingBulk() {
    if (!wifiConfig.isConnected() || pendingCount == 0) return;
    if (isPeakHour()) {
        Serial.printf("[Bulk] PEAK HOUR — deferring upload (%d queued)\n", pendingCount);
        return;
    }

    Serial.printf("[Bulk] Uploading %d queued record(s) in one request...\n", pendingCount);

    DynamicJsonDocument doc(pendingCount * 200 + 256);
    doc["device_id"] = deviceId;
    JsonArray arr = doc.createNestedArray("records");
    for (int i = 0; i < pendingCount; i++) {
        JsonObject o = arr.createNestedObject();
        o["employee_uid"] = pendingQueue[i].empUid;
        o["nfc_uid"]      = pendingQueue[i].nfcUid;
        o["clock_type"]   = pendingQueue[i].clockType;
        o["clock_time"]   = pendingQueue[i].timestamp;
        o["date"]         = pendingQueue[i].date;
    }
    String payload;
    serializeJson(doc, payload);

    DynamicJsonDocument resp(pendingCount * 150 + 512);
    bool ok = attService.postBulkAttendance(payload, resp);

    if (!ok) {
        // Whole batch failed (network drop mid-request, server error, etc.)
        // — nothing confirmed, so leave the queue exactly as-is for the next
        // trigger. No blanket retryCount bump here: a transport-level failure
        // isn't any one record's fault, so it shouldn't count against them
        // individually the way a per-record server rejection does below.
        Serial.println("[Bulk] Upload failed — queue unchanged, will retry next trigger");
        return;
    }

    // Server delegates this whole batch to syncAttendanceRecords() (see
    // bulk-record-endpoint.php), so the response shape is THAT function's:
    //   { processed_count, duplicate_count, error_count, errors: [{index,...}] }
    // — not a per-record status list. Only failures are individually
    // indexed; anything NOT in errors[] was either inserted OR recognized
    // as a duplicate, and either way the server already has it, so it's
    // safe to drop from the local queue (duplicate == pointless to retry,
    // same reasoning as recordAttendance()'s 409 handling elsewhere).
    //
    // static, not stack-local — same reasoning as flushPending()'s `keep[]`:
    // at PENDING_QUEUE_CAP=200 * 5 Strings/record this is too big for the
    // uploadWorker task's 12KB stack, and this function only ever runs on
    // that one task, so a static buffer is safe to reuse.
    static PendingRecord keep[PENDING_QUEUE_CAP];
    static bool          failedIdx[PENDING_QUEUE_CAP];
    for (int i = 0; i < pendingCount; i++) failedIdx[i] = false;

    if (resp.containsKey("errors") && !resp["errors"].isNull()) {
        for (JsonObject e : resp["errors"].as<JsonArray>()) {
            int idx = e["index"] | -1;
            if (idx >= 0 && idx < pendingCount) failedIdx[idx] = true;
        }
    }

    int kept = 0, resolved = 0, quarantined = 0;
    for (int i = 0; i < pendingCount; i++) {
        if (!failedIdx[i]) { resolved++; continue; }   // inserted or duplicate — drop

        // Failed — keep it, bump retryCount, same cap as flushPending()
        pendingQueue[i].retryCount++;
        if (pendingQueue[i].retryCount >= MAX_RETRIES_BEFORE_QUARANTINE) {
            quarantined++;
            Serial.printf("[Bulk] ⚠ QUARANTINED after %d failed attempts: %s %s\n",
                          pendingQueue[i].retryCount,
                          pendingQueue[i].empUid.c_str(),
                          pendingQueue[i].clockType.c_str());
            SDLockGuard _sdLock;   // see sd_mutex.h
            File qf = SD_MMC.open("/logs/quarantined_records.log", FILE_APPEND);
            if (qf) {
                char nowStr[24];
                snprintf(nowStr, sizeof(nowStr), "%s %02d:%02d:%02d",
                         dateStr().c_str(), clkH, clkM, clkS);
                qf.printf("[%s] empUid=%s clockType=%s timestamp=%s date=%s retries=%d\n",
                          nowStr, pendingQueue[i].empUid.c_str(),
                          pendingQueue[i].clockType.c_str(),
                          pendingQueue[i].timestamp.c_str(),
                          pendingQueue[i].date.c_str(),
                          pendingQueue[i].retryCount);
                qf.close();
            }
            continue;   // dropped, not kept
        }
        keep[kept++] = pendingQueue[i];
    }

    for (int i = 0; i < kept; i++) pendingQueue[i] = keep[i];
    pendingCount = kept;
    savePendingQueueToSD();

    Serial.printf("[Bulk] Done: %d resolved (inserted/duplicate), %d quarantined, %d remain\n",
                  resolved, quarantined, pendingCount);
}

static void uploadWorkerTask(void* /*param*/) {
    for (;;) {
        // Wait for loop() to signal that an upload is needed
        if (xSemaphoreTake(g_uploadTrigger, portMAX_DELAY) == pdTRUE) {
            // One bulk POST drains the whole queue in a single request — see
            // flushPendingBulk() above. Replaces the old one-record-per-call
            // loop (flushPending() is still available/used elsewhere as a
            // single-record fallback, just no longer the hot path here).
            // Wait for the seed worker to finish if it's mid-pass, then hold
            // the mutex for the whole drain so seed can't start underneath us
            // either — see g_networkMutex comment above uploadWorkerTask.
            if (g_networkMutex) xSemaphoreTake(g_networkMutex, portMAX_DELAY);
            if (wifiConfig.isConnected() && pendingCount > 0 && !isPeakHour()) {
                flushPendingBulk();
            }
            if (g_networkMutex) xSemaphoreGive(g_networkMutex);
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
            if (hwm < 512) {
                Serial.printf("[Upload] ⚠ low stack headroom: %u bytes free (task stack may be too small)\n",
                              (unsigned)(hwm * sizeof(StackType_t)));
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
    // Belt-and-suspenders: force full brightness before the profile/photo
    // ever gets drawn, regardless of what state the backlight was already
    // in. wakeScreen() below already does this when screenIsOff, but this
    // guarantees the employee's photo is never shown at anything less than
    // full brightness even in an edge case that isn't screenIsOff.
    TFTDisplayManager::backlightOn();

    // ── Wake display / refresh clock immediately ──────────────────────────
    if (screenIsOff) {
        wakeScreen();   // full restore: backlight + drawStaticUI + clock
    } else {
        // Screen already on — repaint clock right now before any blocking
        // SD / server work, so the display never appears frozen mid-scan
        if (clockIsSynced()) updateClock(clkH, clkM, clkS);
        else                 handleUnsyncedClockOrReboot(false);
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

    // STEP 1b: Short-read fallback. cardIdentifier can be a truncated ID
    // (card left the RF field mid-read — see nfc_manager.cpp) when it's
    // shorter than MIN_CARD_ID_LEN but still passed isAllDigits() and
    // MIN_PREFIX_LEN in the NFC poll gate. Only try this if the exact
    // lookup above missed; loadUidForNfcPrefix() itself refuses to guess
    // (returns "" unless exactly one cached employee matches the prefix),
    // so this never risks clocking in the wrong person — it only rescues
    // reads that are unambiguous against what's already cached on SD.
    if (empUid.length() == 0 && cardIdentifier.length() < MIN_CARD_ID_LEN) {
        empUid = SDDatabase::loadUidForNfcPrefix(cardIdentifier);
        if (empUid.length() > 0) {
            Serial.println("[NFC] Short read '" + cardIdentifier +
                            "' accepted via unique prefix match → uid=" + empUid);
        }
    }

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
        // A short read (< MIN_CARD_ID_LEN) that didn't uniquely match a
        // cached employee in STEP 1b has no full ID to send — the server
        // compares against the full id_number, so a truncated string is a
        // guaranteed-fail round trip (this is exactly what MIN_CARD_ID_LEN
        // was raised to avoid). Fail locally instead and let the person
        // re-tap; this only affects employees not yet cached on this
        // device (e.g. their very first scan here).
        if (cardIdentifier.length() < MIN_CARD_ID_LEN) {
            Serial.println("[NFC] Short read '" + cardIdentifier +
                            "' — no unique local match, refusing server round trip");
            empDisplay->showError("Read failed\nTap again");
            enterState(STATE_NFC_ERROR);
            return;
        }
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
        bool commError = false;
        bool granted = attService.authenticateNFC(cardIdentifier, deviceId, emp, &commError);
        if (!granted) {
            if (commError) {
                // We couldn't reach/parse the server — this is NOT a
                // registration verdict, so don't tell the employee their
                // card isn't registered. See authenticateNFC()'s comment.
                empDisplay->showError("Connection Error\nTap again");
            } else {
                empDisplay->showError(emp.hasData ? "Access Denied" : "Card Not Registered");
            }
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

    // ── STEP 7b: Raw tap log (debug-only, separate from /attendance/) ────
    // Every physical tap that reaches this point, unconditionally — name +
    // NFC UID + time — so a mis-dated or mis-typed attendance row can be
    // cross-checked against "what did the reader actually see, and when".
    // Lives in /tap_log/, its own per-date file, never synced to the server
    // and never touched by the Attendance Editor (that only edits the
    // server DB via attendance.php, which has no route to this file).
    if (SDDatabase::isReady()) {
        SDDatabase::logRawTap(ts, emp.fullName, cardIdentifier);
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
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
            if (hwm < 512) {
                Serial.printf("[NFC] ⚠ low stack headroom: %u bytes free (task stack may be too small)\n",
                              (unsigned)(hwm * sizeof(StackType_t)));
            }
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

// ─── loopTask stack size override ──────────────────────────────────────────
// The Arduino-ESP32 core sizes and creates the main "loopTask" (which runs
// setup()+loop()) INSIDE app_main(), before setup() ever executes — by
// calling this exact weak function. The default is only 8192 bytes, which
// is too tight for seedTodayAttendanceFromServer()'s HTTPClient +
// ArduinoJson + AES decrypt of large JSON payloads — the same stack-overflow
// class already fixed for nfcWorker/uploadWorker/seedWorker (see their
// xTaskCreatePinnedToCore stack sizes below), but triggerInitialSync() calls
// seedTodayAttendanceFromServer() synchronously on THIS task at boot, before
// the seedWorker task (and its 16KB stack) exists yet — leaving this one
// call still running on the thin 8KB stack. Matches the last_crash.log
// panic: uptime ~121s (mid initial sync/AES-decrypt window), heap/psram
// healthy — consistent with a stack overflow, not an OOM.
// NOTE: overriding the weak function is the only way to change this — by
// the time setup() runs, the task (and its stack) already exists, so a
// runtime "setter" call from inside setup() would be too late.
size_t getArduinoLoopTaskStackSize(void) {
    return 16384;
}

// ─── setup ───────────────────────────────────────────────────────────────────
// Fast boot: no long delays between steps. All steps are sequential but each
// one is as tight as possible. Target: ready for NFC taps within 4 seconds.
void setup() {
    Serial.begin(115200);
    // Wait for the host to actually attach to the USB-CDC port (up to 3s),
    // instead of a fixed 100ms — on ESP32-S3 native USB, 100ms often isn't
    // enough for the monitor to reopen the port after enumeration, so the
    // earliest Serial.println() calls in setup() were getting dropped
    // before anything was listening. If nothing ever attaches (e.g. no
    // monitor open, running on battery), this still falls through after
    // 3s so boot isn't blocked forever.
    uint32_t _serialWaitStart = millis();
    while (!Serial && millis() - _serialWaitStart < 3000) delay(10);
    delay(200); // small settle margin after the host connects

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
    // Must exist before SDDatabase::begin() and before any worker task
    // (nfc/upload/seed) is created — all three touch the SD card and were
    // previously unsynchronized, which is the root cause of the long-idle
    // NFC freeze. See sd_mutex.h for details.
    sdMutexInit();
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
    screensaverInit();   // checks for /logo.jpg — safe to call even if SD failed

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
        // Guards against the "clock shows a stopwatch instead of the real
        // time" symptom — see ensureClockSyncedOrReboot()'s comment above.
        // Reboots (a bounded number of times) instead of proceeding into the
        // dashboard with an un-synced clock if NTP didn't land.
        ensureClockSyncedOrReboot();
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

    // Portal delete/edit handlers (WiFiManager.h) can't see this file's
    // `screenIsOff` (it's static/file-local) and must never draw to the TFT
    // directly from an HTTP handler — see g_requestDashboardRefresh's doc
    // comment in WiFiManager.h. Route them through the same deferred-flag
    // mechanism loop() already uses for every other sync-trigger.
    g_requestDashboardRefresh = []() {
        g_pendingLastScanClear = true;
        g_pendingStatsRefresh  = true;
    };

    // Lets the Actions tab's Awake/Asleep toggle poll /api/status and stay
    // in sync with reality — including timeouts/wakes that didn't originate
    // from the portal (an NFC tap, the normal 5-min idle timeout, etc.).
    g_isScreenOff = []() { return screenIsOff; };

    // Lets the Dashboard's "Pending Sync" stat tile report the real
    // not-yet-uploaded queue depth via /api/status.
    g_getPendingCount = []() { return pendingCount; };

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

    // Created once, before the upload/seed workers exist, so it's always
    // ready by the time either task's first trigger can fire.
    g_networkMutex = xSemaphoreCreateMutex();

    // ── 6b. Seed worker task (Core 0) — started BEFORE triggerInitialSync() ──
    // Runs seedTodayAttendanceFromServer() (2-3 paginated HTTP calls, 5-10s)
    // off Core 1 so a WiFi-reconnect mid-rush or a portal "Reseed Today"
    // click never freezes NFC polling. loop() signals via g_seedTrigger.
    //
    // FIX (crash-on-boot / reboot loop): this task must exist BEFORE the
    // very first triggerInitialSync() call below. requestSeedToday() falls
    // back to calling seedTodayAttendanceFromServer() synchronously, on
    // Core 1, inside setup(), whenever g_seedTrigger/g_seedTask don't exist
    // yet. That synchronous HTTPS call was hitting lwIP's TCPIP core too
    // early in boot (before it's ready to service a call made outside its
    // own locked task context), causing:
    //   assert failed: udp_new_ip_type ... "Required to lock TCPIP core
    //   functionality!"
    // which panics and reboots, then crashes again at the same spot on the
    // next boot — a fast, silent-looking crash loop. Creating the seed
    // worker task first means the boot-time seed request goes through the
    // normal async/queued path (deferred to Core 0, after the system has
    // fully settled) instead of running synchronously inside setup().
    g_seedTrigger = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(
        seedWorkerTask,    // task function
        "seedWorker",      // name
        // This task does the heaviest work of the three — paginated HTTP +
        // AES decrypt of ~40KB+ JSON payloads (see [AES] PASS 1 in the boot
        // log) — so it gets the most headroom (see nfcWorker note below).
        16384,             // stack — enough for HTTP + JSON
        nullptr,           // parameter
        1,                 // priority (same as uploadWorker)
        &g_seedTask,       // handle
        0                  // Core 0 — keeps HTTP off Core 1
    );
    Serial.println("[Ready] Seed worker task started on Core 0");

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
        // 8192 was tight for HTTPClient + ArduinoJson + String concatenation
        // on ESP32 (a common source of an unexplained PANIC/TASK_WDT reset
        // with otherwise-healthy heap/PSRAM, like the one in last_crash.log —
        // see resetReasonStr() in sd_logger.h for how to confirm the cause
        // next time). Bumped for headroom; watch stack high-water marks.
        12288,           // stack (bytes) — enough for JSON + HTTP
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
        12288,             // stack — enough for HTTP + JSON (see nfcWorker note above)
        nullptr,           // parameter
        1,                 // priority (same as loop, lower than nfcWorker)
        &g_uploadTask,     // handle
        0                  // Core 0 — keeps HTTP off Core 1
    );
    Serial.println("[Ready] Upload worker task started on Core 0");

    // ── 9. Seed worker task ──────────────────────────────────────────────────
    // (moved earlier — see "Seed worker task (Core 0) — started BEFORE
    // triggerInitialSync()" above step 6b — so it exists before the
    // boot-time triggerInitialSync() call that needs it.)

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
    static unsigned long lastApScheduleCheck = 0;
    static unsigned long reconcilePollRateMs = RECONCILE_POLL_BASE_MS;
    static int            reconcileErrorCount = 0;
    static uint8_t       tick            = 0;
    unsigned long now = millis();

    wifiConfig.handleClient();
    wifiManager.handleClient();

    // ── Setup hotspot (softAP) schedule — same 08:00-17:00 admin window as
    // the idle reconcile burst just below (isAdminWorkingHours()), reused
    // rather than a separate window: it's the same underlying reason ("no
    // admin around to need either one"), so one schedule covers both instead
    // of maintaining two near-identical windows. Does NOT affect uploads —
    // the immediate background-upload trigger on screen-off (further down)
    // and the background upload scheduler both stay on their own always-on
    // schedule, since an employee can clock in well after 17:00 and that
    // record still needs to reach the server promptly. Only the idle
    // reconcile polling and this hotspot get throttled outside admin hours.
    // The STA connection to the office network (what the portal is actually
    // browsed over — see 192.168.1.72 in the screenshots) is untouched
    // either way; this only toggles the device's own setup AP. Checked once
    // a minute rather than every loop() — startAP()/stopAP() already no-op
    // if already in the desired state, but there's no reason to even make
    // that check thousands of times a second.
    if (now - lastApScheduleCheck >= 60000) {
        lastApScheduleCheck = now;
        // clkEpoch == 0 means the clock hasn't synced yet (no STA connection
        // yet, or NTP hasn't landed) — clkH/clkM would still read 00:00 in
        // that state, which would read as "outside admin hours" and shut the
        // one hotspot someone needs to initially configure the device over.
        // Leave the AP alone until we have a real clock to judge by.
        bool shouldBeOn = (clkEpoch == 0) || isAdminWorkingHours();
        if (shouldBeOn && !wifiConfig.isAPActive()) {
            wifiConfig.startAP();
        } else if (!shouldBeOn && wifiConfig.isAPActive()) {
            wifiConfig.stopAP();
        }
    }

    // ── Portal-triggered wake/sleep ─────────────────────────────────────────
    // Set by the Actions tab's "Wake Screen" / "Sleep Screen" buttons
    // (WiFiManager.h's _apiScreenWake()/_apiScreenSleep()), checked here once
    // per loop so the actual TFT/state work stays on the same core/loop as
    // everything else that touches the display.
    if (g_triggerScreenWake) {
        g_triggerScreenWake = false;
        if (screenIsOff) {
            wakeScreen();
            Serial.println("[Screen] Portal — manual wake");
        }
    }
    if (g_triggerScreenSleep) {
        g_triggerScreenSleep = false;
        // Only forces the screensaver from the live dashboard — never fights
        // an in-progress NFC tap/profile display, wifi setup, etc.
        if (!screenIsOff && currentState == STATE_DASHBOARD) {
            lastActivityMs = now - SCREEN_TIMEOUT_MS;   // let the block below fire this tick
            Serial.println("[Screen] Portal — manual sleep");
        }
    }

    // wakeScreen()/the sleep branch above both just touched lastActivityMs
    // using a *fresh* millis() call — `now` above was captured before that,
    // so it can be < lastActivityMs. Since both are unsigned long, the
    // timeout check just below (now - lastActivityMs) would then underflow
    // into a huge number and immediately re-trigger sleep on this same
    // tick — which is exactly why "Wake" used to flash on for ~0.5s and
    // instantly go back to the screensaver. Re-sampling now here keeps the
    // subtraction below always non-negative.
    now = millis();

    // ── Rush-mode edge detection (major clock-out lines) ────────────────────
    // See RUSH_WINDOWS above. On the rising edge we force-wake the screen
    // ahead of the line and hold it awake for the whole window (screen
    // timeout is gated below). On the falling edge we run the deferred
    // background work that was intentionally skipped during the rush —
    // flush the pending upload queue, refresh the server-side cache via
    // requestSeedToday(), and reset idle-poll state — in that order, one
    // step at a time, rather than letting several tasks all wake up and
    // fire at once right as the line clears.
    static bool wasRushPrewakeActive = false;
    bool rushPrewakeNow = isRushPrewakeActive();
    if (rushPrewakeNow && !wasRushPrewakeActive) {
        Serial.println("[Rush] Entering rush window — waking screen, holding it awake");
        if (screenIsOff) wakeScreen();
        else lastActivityMs = now;   // hold it awake even if already on
    }
    if (!rushPrewakeNow && wasRushPrewakeActive) {
        Serial.println("[Rush] Rush window ended — running deferred maintenance");
        // STEP 1: flush anything that piled up in the pending upload queue
        // while background uploads were paused by isPeakHour().
        if (wifiConfig.isConnected() && pendingCount > 0 && !isPeakHour()) {
            lastUploadFlush = now;
            if (g_uploadTrigger) xSemaphoreGive(g_uploadTrigger);
        }
        // STEP 2: refresh the server-side cache (SD mirror) now that the
        // rush's own writes have landed. uploadWorkerTask/seedWorkerTask
        // share g_networkMutex, so this naturally waits its turn behind
        // STEP 1's flush instead of racing it.
        if (wifiConfig.isConnected() && !isPeakHour()) {
            requestSeedToday();
        }
        // STEP 3: fresh idle-poll state so background reconcile polling
        // resumes cleanly (a full burst of IDLE_RECONCILE_BURST_COUNT)
        // once the screen naturally times out again, rather than resuming
        // mid-burst from before the rush started.
        g_idlePollCount  = 0;
        g_idleResting    = false;
        g_lastIdlePollMs = now;
        lastActivityMs   = now;   // let the normal timeout start fresh from here
    }
    wasRushPrewakeActive = rushPrewakeNow;

    // ── Screen timeout ────────────────────────────────────────────────────
    if (!screenIsOff &&
        currentState == STATE_DASHBOARD &&
        !rushPrewakeNow &&
        (now - lastActivityMs >= SCREEN_TIMEOUT_MS)) {
        screenIsOff = true;
        // Backlight stays ON: on this board TFT_BL only gates the LED, the
        // ST7789 panel itself keeps a faint glow visible either way, so a
        // "blanked" screen actually looked like a dim gray rectangle. Showing
        // the logo/clock screensaver instead is strictly better and costs no
        // extra power over the old dim-gray state.
        drawScreensaver();
        g_screensaverEnteredMs = now;
        g_screensaverDimmed    = false;
        Serial.println("[Screen] Timeout — showing screensaver");

        // ── Trigger upload immediately on screen-off ──────────────────────
        // Instead of waiting up to 10 minutes for the upload timer, kick off
        // the background worker as soon as the screen blanks. The worker runs
        // on Core 0 so it never blocks NFC or the clock.
        if (wifiConfig.isConnected() && pendingCount > 0 && !isPeakHour()) {
            Serial.println("[Screen] Screen off — triggering immediate background upload");
            lastUploadFlush = now;   // reset the timer so the next fire is a full 10 min later
            if (g_uploadTrigger) xSemaphoreGive(g_uploadTrigger);
        }

        // Idle burst starts clean the moment the screen goes dark.
        g_idlePollCount  = 0;
        g_idleResting    = false;
        g_lastIdlePollMs = now;   // first poll fires after one full interval, not instantly
    }

    // ── Screensaver burn-in protection: dim after N seconds ────────────────
    // Re-enabled per request. Note the original tradeoff still applies:
    // fadeBacklight() blocks loop() (and therefore NFC polling) for up to
    // ~800ms while it steps the backlight down. In practice a tap landing
    // in that exact window is rare, and full brightness for the actual
    // profile/photo screen is unaffected either way (nfcWorkerBody() always
    // forces backlightOn() before drawing a profile, dim or not).
    if (screenIsOff && !g_screensaverDimmed &&
        (now - g_screensaverEnteredMs >= DIM_AFTER_SCREENSAVER_MS)) {
        if (TFTDisplayManager::fadeBacklight(SCREENSAVER_DIM_LEVEL, 800)) {
            g_screensaverDimmed = true;
            Serial.println("[Screen] Screensaver dimmed (burn-in protection)");
        }
    }

    // ── Idle reconcile burst (screen-off polling for portal edits/deletes) ─
    // Only runs while the screen is actually off — an NFC tap calls
    // wakeScreen(), which flips screenIsOff back to false and resets the
    // burst state, so this block simply stops firing on the very next loop()
    // iteration. No separate "cancel" check needed.
    //
    // Behavior: poll up to IDLE_RECONCILE_BURST_COUNT times, spaced
    // IDLE_RECONCILE_INTERVAL_MS apart, then rest IDLE_RECONCILE_REST_MS
    // before starting the next burst — so a device left idle for hours
    // (overnight, weekends) doesn't hammer itself or the server forever.
    // Also gated on isAdminWorkingHours() (08:00–17:00): the admin only
    // edits/deletes on the portal during that window, so there's nothing to
    // pull outside it — the whole block simply goes dormant once the clock
    // passes 17:00 (even mid-burst or mid-rest) and picks back up at 08:00
    // the next day, no separate reset logic needed since the check runs
    // live every loop() iteration.
    // Same isConnected/isPeakHour/heap/g_seedRunning guards as the existing
    // 30-min "away" reconcile poller below, since this shares the same
    // requestSeedToday() -> seedWorkerTask() pipeline; requestSeedToday()
    // already stamps g_seedFinishedAtMs on completion, which also keeps the
    // 30-min poller's timer from double-firing moments later.
    if (screenIsOff &&
        wifiConfig.isConnected() && currentState == STATE_DASHBOARD &&
        isAdminWorkingHours() && !isPeakHour() && !g_seedRunning) {

        if (g_idleResting) {
            if (now - g_idleRestStartedAtMs >= IDLE_RECONCILE_REST_MS) {
                g_idleResting    = false;
                g_idlePollCount  = 0;
                g_lastIdlePollMs = now;   // next burst's first poll after one interval
                Serial.println("[IdlePoll] Rest complete — starting next burst");
            }
        } else if (g_idlePollCount < IDLE_RECONCILE_BURST_COUNT &&
                   (now - g_lastIdlePollMs >= IDLE_RECONCILE_INTERVAL_MS)) {
            g_lastIdlePollMs = now;
            if (ESP.getFreeHeap() > 40000) {
                g_idlePollCount++;
                Serial.printf("[IdlePoll] Poll %d/%d (screen idle)\n",
                              g_idlePollCount, IDLE_RECONCILE_BURST_COUNT);
                requestSeedToday();

                if (g_idlePollCount >= IDLE_RECONCILE_BURST_COUNT) {
                    g_idleResting         = true;
                    g_idleRestStartedAtMs = now;
                    Serial.println("[IdlePoll] Burst complete — resting 30 min before next burst");
                }
            }
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
        // Screensaver-corruption guard — see loop()'s sync-trigger blocks.
        if (!screenIsOff) {
            updateStatusDots(false, SDDatabase::isReady(), true);
        }
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
                // First time the clock is synced this boot. Check the SD-
                // persisted date from before the last shutdown/reboot — if
                // it's stale (a prior day), that rollover was missed live
                // (e.g. the device rebooted overnight) and its files were
                // never queued for purge. Catch up on it now.
                String savedDate = loadLastSeenDateFromSD();
                if (savedDate.length() == 10 && savedDate != todayStr) {
                    Serial.printf("[Rollover] Boot catch-up: stale date %s found on SD (today=%s) — queuing purge\n",
                                  savedDate.c_str(), todayStr.c_str());
                    g_cleanupPendingDate = savedDate;
                }
                g_lastSeenDate = todayStr;   // anchor for live rollover detection going forward
                saveLastSeenDateToSD(todayStr);
            } else if (todayStr != g_lastSeenDate) {
                Serial.printf("[Rollover] Date changed %s -> %s\n",
                              g_lastSeenDate.c_str(), todayStr.c_str());
                g_cleanupPendingDate = g_lastSeenDate;  // yesterday — purge once fully synced
                g_lastSeenDate = todayStr;
                saveLastSeenDateToSD(todayStr);
                // New day = new CSV file (todayFilename() rolls over with
                // dateStr()). The cached check-in/check-out counters were
                // counting rows in yesterday's file, so they'd silently
                // report yesterday's numbers against today's (empty) file
                // otherwise — drop them so the next dashboard stat pull does
                // one fresh scan of the new file and reprimes correctly.
                SDDatabase::resetTodayCountCache();

                // Same gmtime()-vs-localtime() pitfall as dateStr() above —
                // use local calendar day, not UTC, to decide "is it Monday".
                time_t t = (time_t)clkEpoch;
                struct tm tmNow;
                localtime_r(&t, &tmNow);
                if (tmNow.tm_wday == 1) {  // Monday
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
        // Only repaint the dashboard chrome if the dashboard is actually what's
        // on screen right now. drawStaticUI()/updateStatusDots() draw directly
        // onto whatever is currently displayed — if the screensaver is showing
        // (screenIsOff == true), calling them here paints dashboard labels and
        // status badges straight on top of the screensaver, corrupting it into
        // a hybrid mess that never gets cleaned up (the screensaver only ever
        // repaints its own small clock/date regions, never a full fillScreen).
        // wakeScreen() already does a full drawStaticUI()+refresh the moment
        // the screensaver ends, so it's safe to just skip the repaint here.
        if (!screenIsOff) {
            drawStaticUI();
            updateStatusDots(isConnected, SDDatabase::isReady(), true);
        }
    }

    // ── Manual action triggers from web portal ────────────────────────────
    if (g_triggerEmployeeSync) {
        g_triggerEmployeeSync = false;
        if (isConnected && currentState == STATE_DASHBOARD) {
            Serial.println("[Action] Manual employee sync triggered");
            EmployeeSync::fullSyncIfNeeded(attService, true);
            // Same screensaver-corruption guard as the weekly re-sync above.
            if (!screenIsOff) {
                drawStaticUI();
                updateStatusDots(isConnected, SDDatabase::isReady(), true);
            }
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
            // KNOWN REMAINING ISSUE: unlike seed/upload, this still runs
            // directly in loop() on Core 1 — it blocks NFC polling and the
            // clock for the whole walk + every HTTP photo download. It's
            // manually triggered only (not part of the idle/reconcile path
            // that causes the long-idle freeze), so it's lower priority, but
            // it should eventually move to its own worker task the same way
            // seedWorkerTask/uploadWorkerTask were split out. Left as-is for
            // this pass — flagging so it isn't mistaken for already fixed.
            SDLockGuard _sdLock;   // guards the directory walk itself; see sd_mutex.h.
            // NOTE: this lock is held across the HTTP downloads below too,
            // since the directory File handle (root/f) must stay open across
            // them — an intentional trade-off for now: a manual photo sync
            // is rare/admin-initiated, whereas the seed pass we fixed earlier
            // runs automatically and repeatedly during idle, which is what
            // actually causes the reported freeze.
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
    // Every reconcilePollRateMs (base 30 min) AFTER THE PREVIOUS RUN FINISHED,
    // asks the seed worker to re-run PASS 1-3 so a row deleted on the portal
    // disappears from the device without needing a reboot/WiFi-blip/manual
    // Reseed. Gated on g_seedFinishedAtMs (not dispatch time) and !g_seedRunning
    // because a full pass can take 30-50s+ with verbose per-employee logging.
    // Gating on dispatch time let 2-3 poll ticks land mid-run; each queued a
    // semaphore give, so the worker started its next pass with zero gap the
    // instant the current one finished — the "log never stops" symptom.
    // Same idle/peak/heap guards as the socket poller above; the request
    // itself runs on Core 0 (seedWorkerTask) so it never blocks NFC polling
    // on Core 1. Base bumped from 15s → 30 min: at 15s the repeated HTTP+JSON
    // churn was eating into the ESP32's free heap and freezing the dashboard
    // UI, especially when NFC taps were also coming in rapid-fire during
    // morning/noon/afternoon "out" lines.
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
            // Screensaver-corruption guard — see loop()'s sync-trigger blocks.
            // The portal still gets the real numbers below either way; only
            // the on-screen stat cards are skipped while the screensaver is up.
            if (!screenIsOff) updateAttendanceStats(ins, outs);
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
            unsigned long displayMs = PROFILE_DISPLAY_MS;   // same value, peak or not — see define above
            // Display duration only matters if NO one else taps: it resets
            // every time a fresh profile appears, so an employee's photo
            // stays up as long as the reader stays quiet. A buffered
            // next-card always wins immediately and swaps straight into the
            // new profile — no intermediate dashboard in between. FIX: the
            // old code called drawStaticUI() (painting the full dashboard)
            // and THEN immediately called handleNFCDetected() for the
            // buffered card, which overwrote it with the loading screen a
            // moment later — that showed up as a visible flash of the
            // dashboard on a fast-moving line. Now a swap skips the
            // dashboard redraw entirely.
            bool nextCardReady = (_nextPendingCard.length() > 0);

            if (nextCardReady) {
                String buffered = _nextPendingCard;
                _nextPendingCard = "";
                Serial.println("[NFC] Processing buffered scan-ahead card: " + buffered);
                handleNFCDetected(buffered);
            } else if (stateElapsed() >= displayMs) {
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
            }
            break;
        }

        case STATE_NFC_ERROR:
            if (stateElapsed() >= NFC_ERROR_DISPLAY_MS) {
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
            if (!screenIsOff) {
                pulseStatus(tick % 2);
                if (clockIsSynced()) updateClock(clkH, clkM, clkS);
                else                 handleUnsyncedClockOrReboot(false);
                if (tick % 60   == 0) updateDate(buildDateStr());
            } else {                                      // screensaver is showing
                if (clockIsSynced()) updateScreensaverClock(clkH, clkM, clkS);
                else                 handleUnsyncedClockOrReboot(true);
                if (tick % 60   == 0) updateScreensaverDate(buildDateStr());
            }
            if (tick % 3600 == 0 && isConnected) syncNTPTime();

            if (clkH == 0 && clkM == 0 && clkS == 0) {
                Serial.println("[Midnight] New day — resetting stats");
                peakEndMin = 0xFFFF;  // reset peak tracking at midnight
                SDDatabase::setDateProvider([]() -> String { return dateStr(); });
                // Same screensaver-corruption guard as the sync triggers above —
                // don't paint the dashboard chrome over an active screensaver.
                // The data reset (stats, date provider) still happens either way;
                // only the on-screen repaint is conditional.
                if (!screenIsOff) {
                    drawStaticUI();
                    updateStatusDots(isConnected, SDDatabase::isReady(), true);
                    updateAttendanceStats(0, 0);
                    updateDate(buildDateStr());
                } else {
                    updateScreensaverDate(buildDateStr());
                }
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
        showChangeNotice("ATTENDANCE UPDATED");
    }

    // Auto-hides the 5s toast started by showChangeNotice() (portal delete,
    // or a background reconcile pass removing a server-deleted row above).
    tickChangeNotice();

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
            // Added isAllDigits(): a garbled read that's still >= MIN_CARD_ID_LEN
            // long (e.g. '2400!41283567' — right length, one glitched byte) used
            // to sail through this check and go all the way to a doomed SD/server
            // lookup. Catching it here means that round trip never happens —
            // the very next poll (30ms later) usually reads the same still-
            // present card cleanly instead.
            //
            // Reads between MIN_PREFIX_LEN and MIN_CARD_ID_LEN are also let
            // through now: these are the truncated-by-early-liftoff reads
            // (see nfc_manager.cpp) rather than mid-string glitches, since a
            // dropped RF field cuts the tag END, not the middle. STEP 1b in
            // nfcWorkerBody() only accepts one of these if it uniquely
            // matches a single cached employee's ID prefix — anything
            // ambiguous or unknown still fails safe and prompts a re-tap.
            if (nfcData.length() >= MIN_PREFIX_LEN && isAllDigits(nfcData)) {
                cardId = nfcData;
            } else {
                // Card is physically present (PN532 saw it) but the NDEF
                // payload didn't come back — nfcUID used to be sent to the
                // server as a fallback "cardId" here, but nfcUID is the raw
                // 7-byte hardware UID (e.g. "04:13:34:24:23:02:89"), which
                // is never what's registered in emp_list.nfc_access (that's
                // always the NDEF text payload, e.g. "2404141283567"). So
                // every fallback attempt was a guaranteed-to-fail HTTPS
                // round trip to /api/nfc-auth that showed the person
                // "Access Denied" for what was actually just a bad read —
                // see nfc-access.php's log: "NFC Auth Failed:
                // UID=04:13:34:24:23:02:89" repeating on retaps of a card
                // that authenticates fine once the NDEF read succeeds.
                // Treat it as a failed read instead: show a local retry
                // prompt and skip the network call entirely — no server
                // round trip can ever succeed with the raw UID anyway.
                if (currentState == STATE_DASHBOARD) {
                    // BUG FIX: this is the one NFC error path that draws
                    // directly here on Core 1 instead of going through
                    // nfcWorkerBody() (which always wakeScreen()s first) —
                    // a fast tap that drops the card before the NDEF text
                    // finishes reading lands exactly here. Without this
                    // wake, showError() painted the error card straight on
                    // top of a still-dimmed screensaver (never cleared,
                    // never brought back to full brightness) — the
                    // overlap/stuck-dim bug. Rule: any NFC activity —
                    // success OR failure — wakes the screen first.
                    if (screenIsOff) wakeScreen();
                    empDisplay->showError("Read failed\nTap again");
                    enterState(STATE_NFC_ERROR);
                    resetScreenTimer();
                }
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
            // Card lifted off the reader before cardConfirmed() ever reached
            // CARD_CONFIRM_NEEDED agreeing reads (a "quick tap and go").
            // BUG FIX: this used to just silently reset the counters here —
            // on a busy line, an employee who taps and immediately walks off
            // got zero feedback and zero attendance record, with nothing in
            // Serial/SD to show a tap even happened. Now an abandoned,
            // never-confirmed tap surfaces the same on-screen error a failed
            // read gets, so the employee sees it didn't register instead of
            // assuming it did. A tap that WAS already confirmed and handed
            // off to handleNFCDetected() (_cardConfirmCt == CARD_CONFIRM_NEEDED)
            // is a normal, successful card lift-off — that case still resets
            // silently since the record was already saved.
            if (_cardConfirmCt > 0) {
                bool wasUnconfirmed = (_cardConfirmCt < CARD_CONFIRM_NEEDED);
                _lastRawCard   = "";
                _cardConfirmCt = 0;
                if (wasUnconfirmed && currentState == STATE_DASHBOARD) {
                    if (screenIsOff) wakeScreen();
                    empDisplay->showError("Tap too quick\nHold ~1 sec");
                    enterState(STATE_NFC_ERROR);
                    resetScreenTimer();
                }
            }
        }
        nfc_poll_end:;
    }
}