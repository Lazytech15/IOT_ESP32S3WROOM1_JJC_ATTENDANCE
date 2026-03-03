// ════════════════════════════════════════════════════════════════════════════
// main.cpp — ESP32-S3 WROOM-1  NFC Attendance  (Portrait 240×320)
//
// KEY ADDITIONS vs previous version:
//   • EmployeeSync::fullSyncIfNeeded() — bulk download all employees + photos
//     on startup (WiFi available) with live TFT progress bar
//   • EmployeeSync::pollChanges()      — polls socket.php every 60 s for
//     employee_created / employee_updated / employee_deleted events and
//     incrementally updates the SD cache (no full re-download needed)
//   • WiFi connect detection: triggers fullSync the first time WiFi comes up
//
// Photo lookup chain (unchanged):
//   NFC tap → cardIdentifier
//   → SD NFC mapping → empUid
//   → load /employees/<empUid>.json  (profilePicture field)
//   → check /photos/<empUid>.jpg on SD
//   → if missing: download using profilePicture path
//   → showEmployeeProfile() with SD photo → JPEG displayed
//
// ════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SD_MMC.h>
#include <time.h>                   // ← NEW: Required for NTP time sync
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
#define NFC_CARD_DISPLAY_MS    2500   // was 4000 (-1.5s)
#define NFC_STATUS_DISPLAY_MS   500   // was 2000 (-1.5s)
#define NFC_POLL_INTERVAL_MS    500
#define CLOCK_UPDATE_MS        1000
#define STATS_REFRESH_MS      30000
#define WIFI_RETRY_MS         15000
#define SYNC_INTERVAL_MS      60000   // push pending offline attendance records
#define SOCKET_POLL_MS        60000   // check server for employee changes

#define AP_SSID     "JJC_Attendance_Config"
#define AP_PASSWORD "ilovejjcenggworks"
#define SERVER_URL  "https://jjcenggworks.com"

String deviceId = "Attendance_Display_01";

// ─── Software clock ──────────────────────────────────────────────────────────
static uint8_t clkH = 0, clkM = 0, clkS = 0;

static void tickClock() {
    if (++clkS >= 60) { clkS = 0;
    if (++clkM >= 60) { clkM = 0;
    if (++clkH >= 24)   clkH = 0; }}
}
static String clockStr() {
    char b[12]; snprintf(b, sizeof(b), "%02d:%02d:%02d", clkH, clkM, clkS);
    return b;
}
static String buildDateStr() {
    static const char* d[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    unsigned long day = millis() / 86400000UL;
    char b[32]; snprintf(b, sizeof(b), "%s - Day %lu", d[day % 7], day);
    return b;
}

// ─── Globals ─────────────────────────────────────────────────────────────────
WiFiConfig            wifiConfig(AP_SSID, AP_PASSWORD);
WiFiManager           wifiManager;
AttendanceHTTPService attService(SERVER_URL);
EmployeeProfileDisplay* empDisplay = nullptr;

// Track whether we've done the initial full sync this session
static bool initialSyncDone = false;

// ─── Pending attendance queue (offline records) ───────────────────────────────
struct PendingRecord {
    String empUid, nfcUid, clockType, ts;
};
static PendingRecord pendingQueue[20];
static int           pendingCount = 0;

static void enqueuePending(const String& empUid, const String& nfcUid,
                            const String& clockType, const String& ts) {
    if (pendingCount < 20) {
        pendingQueue[pendingCount++] = {empUid, nfcUid, clockType, ts};
        Serial.printf("[Queue] +1 pending (%d total)\n", pendingCount);
    }
}
static void flushPending() {
    if (!wifiConfig.isConnected() || pendingCount == 0) return;
    Serial.printf("[Sync] Flushing %d pending records...\n", pendingCount);
    int i = 0, remaining = 0;
    PendingRecord keep[20];
    while (i < pendingCount) {
        bool ok = attService.recordAttendance(
            pendingQueue[i].empUid, pendingQueue[i].nfcUid,
            deviceId, pendingQueue[i].clockType);
        if (!ok) keep[remaining++] = pendingQueue[i];
        else Serial.printf("[Sync] Sent: %s %s\n",
                           pendingQueue[i].empUid.c_str(),
                           pendingQueue[i].clockType.c_str());
        i++;
        yield();
    }
    for (int j = 0; j < remaining; j++) pendingQueue[j] = keep[j];
    pendingCount = remaining;
    Serial.printf("[Sync] Done. %d remain.\n", pendingCount);
}

// ─── State machine ────────────────────────────────────────────────────────────
enum SystemState : uint8_t {
    STATE_DASHBOARD, STATE_NFC_LOADING,
    STATE_NFC_PROFILE, STATE_NFC_SUCCESS, STATE_NFC_ERROR
};
SystemState   currentState   = STATE_DASHBOARD;
unsigned long stateEnteredAt = 0;
String        lastNFCUid     = "";
EmployeeProfile lastEmployee;
String        lastClockType  = "check-in";

static void          enterState(SystemState s) { currentState = s; stateEnteredAt = millis(); }
static unsigned long stateElapsed()            { return millis() - stateEnteredAt; }

// ─── NTP Time Sync ────────────────────────────────────────────────────────────
void syncNTPTime() {
    if (wifiConfig.isConnected()) {
        Serial.println("[Time] Syncing with NTP...");
        // Set timezone to UTC+8 hours
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10000)) { // 10 second timeout
            clkH = timeinfo.tm_hour;
            clkM = timeinfo.tm_min;
            clkS = timeinfo.tm_sec;
            Serial.printf("[Time] Synced: %02d:%02d:%02d\n", clkH, clkM, clkS);
        } else {
            Serial.println("[Time] Failed to obtain time");
        }
    }
}

// ─── downloadAndCachePhoto ────────────────────────────────────────────────────
static String downloadAndCachePhoto(const EmployeeProfile& emp) {
    if (!SDDatabase::isReady())       return "";
    if (emp.uid.length() == 0)        return "";
    if (SDDatabase::hasPhoto(emp.uid))
        return SDDatabase::photoPath(emp.uid);
    if (!wifiConfig.isConnected())    return "";

    Serial.println("[Photo] Downloading uid=" + emp.uid +
                   " path=" + emp.profilePicture);
    uint8_t* buf = nullptr;
    int      len = 0;
    bool ok = attService.downloadProfileImage(emp.uid, &buf, &len,
                                              emp.profilePicture);
    if (!ok || !buf) { Serial.println("[Photo] Download failed"); return ""; }
    bool saved = SDDatabase::savePhoto(emp.uid, buf, (size_t)len);
    free(buf);
    if (saved) {
        Serial.println("[Photo] Cached: " + SDDatabase::photoPath(emp.uid));
        return SDDatabase::photoPath(emp.uid);
    }
    return "";
}

// ─── triggerInitialSync ───────────────────────────────────────────────────────
// Called once when WiFi first connects (or from setup if already connected).
// Shows progress UI on TFT, then redraws the dashboard when done.
static void triggerInitialSync() {
    Serial.println("[Sync] triggerInitialSync() called");
    Serial.flush();

    if (!wifiConfig.isConnected()) {
        Serial.println("[Sync] SKIP: no WiFi");
        return;
    }
    if (initialSyncDone) {
        Serial.println("[Sync] SKIP: already done this session");
        return;
    }
    initialSyncDone = true;

    Serial.println("[Sync] ============================================");
    Serial.println("[Sync] WiFi UP - starting employee sync");
    Serial.printf("[Sync] Heap:  %u B free\n", ESP.getFreeHeap());
    Serial.printf("[Sync] PSRAM: %u B free\n", ESP.getFreePsram());
    Serial.printf("[Sync] SD:    %s\n", SDDatabase::isReady() ? "ready" : "NOT READY");
    Serial.println("[Sync] ============================================");
    Serial.flush();

    // Quick connectivity test - proves ESP32 can reach server
    {
        Serial.println("[Sync] Testing server connection...");
        HTTPClient testHttp;
        testHttp.setTimeout(8000);
        testHttp.begin("https://jjcenggworks.com/api/employees?limit=1&offset=0&includeAllStatuses=true");
        testHttp.addHeader("X-Client-Type", "ESP32");
        int testCode = testHttp.GET();
        String testBody = testHttp.getString();
        testHttp.end();
        Serial.printf("[Sync] Test GET code=%d  len=%d\n", testCode, (int)testBody.length());
        if (testBody.length() > 0)
            Serial.println("[Sync] Test preview: " +
                           testBody.substring(0, min(200,(int)testBody.length())));
        Serial.flush();
    }

    // Force sync only if there are no cached employees yet.
    // If SD already has profiles, skip the full re-download to save time.
    bool hasCache = SDDatabase::isReady() &&
                    SD_MMC.exists("/employees") &&
                    SD_MMC.exists("/employees/sync_meta.json");
    Serial.printf("[Sync] hasCache=%s → force=%s\n",
                  hasCache ? "YES" : "NO",
                  hasCache ? "NO" : "YES");
    Serial.flush();
    EmployeeSync::fullSyncIfNeeded(attService, !hasCache);

    Serial.printf("[Sync] Heap after sync: %u B free\n", ESP.getFreeHeap());
    Serial.println("[Sync] Sync complete");
    Serial.flush();

    drawStaticUI();
    updateStatusDots(wifiConfig.isConnected(), SDDatabase::isReady(), true);
    updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                          max(0, SDDatabase::countTodayCheckOuts()));
}

// ─── handleNFCDetected ────────────────────────────────────────────────────────
static void handleNFCDetected(const String& cardIdentifier) {
    Serial.println("\n================================================");
    Serial.println("[NFC-SCAN] >>> NFC CARD DETECTED <<<");
    Serial.println("[NFC-SCAN] CardIdentifier: " + cardIdentifier);
    Serial.printf("[NFC-SCAN] Heap: %u  PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.printf("[NFC-SCAN] SD ready: %s  WiFi: %s\n",
        SDDatabase::isReady() ? "YES" : "NO",
        wifiConfig.isConnected() ? "YES" : "NO");
    Serial.println("================================================");
    Serial.flush();

    lastNFCUid = cardIdentifier;
    enterState(STATE_NFC_LOADING);
    empDisplay->showLoading();

    EmployeeProfile emp;
    bool fromCache = false;

    // STEP 1: resolve empUid from NFC card identifier
    Serial.println("[STEP-1] Resolving empUid from cardIdentifier...");
    Serial.flush();
    String empUid = SDDatabase::loadUidForNfc(cardIdentifier);
    Serial.println("[STEP-1] loadUidForNfc returned: '" + empUid + "'");
    if (empUid.length() == 0 && SDDatabase::hasEmployeeProfile(cardIdentifier)) {
        Serial.println("[STEP-1] cardIdentifier itself is empUid (direct match)");
        empUid = cardIdentifier;
    }
    if (empUid.length() == 0) {
        Serial.println("[STEP-1] WARNING: empUid is EMPTY - no NFC mapping found on SD!");
    } else {
        Serial.println("[STEP-1] empUid resolved: '" + empUid + "'");
    }
    Serial.flush();

    // STEP 2: load profile from SD cache
    Serial.println("[STEP-2] SD cache lookup for uid='" + empUid + "'");
    Serial.flush();
    if (empUid.length() > 0 && SDDatabase::hasEmployeeProfile(empUid)) {
        Serial.println("[STEP-2] Cache HIT - loading profile...");
        Serial.flush();
        if (SDDatabase::loadEmployeeProfile(empUid, emp)) {
            fromCache = true;
            Serial.println("[STEP-2] Profile loaded OK: '" + emp.fullName + "'");
            Serial.println("[STEP-2]   uid='" + emp.uid + "'  idNumber='" + emp.idNumber + "'");
            Serial.println("[STEP-2]   profilePicture='" + emp.profilePicture + "'");
            Serial.println("[STEP-2]   accessGranted=" + String(emp.accessGranted ? "true" : "false"));
        } else {
            Serial.println("[STEP-2] ERROR: hasEmployeeProfile=true but loadEmployeeProfile FAILED!");
        }
    } else {
        if (empUid.length() == 0)
            Serial.println("[STEP-2] SKIP: empUid is empty - must fetch from server");
        else
            Serial.println("[STEP-2] Cache MISS: /employees/" + empUid + ".json not on SD");
    }
    Serial.printf("[STEP-2] fromCache=%s\n", fromCache ? "YES" : "NO");
    Serial.flush();

    // STEP 3: server lookup (WiFi fallback)
    if (!fromCache) {
        Serial.println("[STEP-3] No local cache - attempting server fetch...");
        Serial.printf("[STEP-3] WiFi connected: %s\n", wifiConfig.isConnected() ? "YES" : "NO");
        Serial.flush();
        if (!wifiConfig.isConnected()) {
            Serial.println("[STEP-3] FAIL: No WiFi - cannot fetch. Showing error.");
            Serial.flush();
            empDisplay->showError("Card not in local cache");
            enterState(STATE_NFC_ERROR);
            return;
        }
        Serial.println("[STEP-3] Calling authenticateNFC: '" + cardIdentifier + "'");
        Serial.flush();
        bool granted = attService.authenticateNFC(cardIdentifier, deviceId, emp);
        Serial.printf("[STEP-3] authenticateNFC result: %s\n", granted ? "GRANTED" : "DENIED/FAILED");
        Serial.printf("[STEP-3] emp.hasData=%s  emp.uid='%s'  emp.fullName='%s'\n",
            emp.hasData ? "true" : "false", emp.uid.c_str(), emp.fullName.c_str());
        Serial.println("[STEP-3] emp.profilePicture='" + emp.profilePicture + "'");
        Serial.flush();
        if (!granted) {
            Serial.println("[STEP-3] Access denied - showing error screen");
            Serial.flush();
            empDisplay->showError(emp.hasData ? "Access denied" : "Card not registered");
            char ts[12]; snprintf(ts, sizeof(ts), "%02d:%02d:%02d", clkH, clkM, clkS);
            emp.clockType = "denied";
            SDDatabase::logAttendance(String(ts), cardIdentifier, emp, "denied", deviceId);
            enterState(STATE_NFC_ERROR);
            return;
        }
        // Cache profile on SD
        if (SDDatabase::isReady() && emp.uid.length() > 0) {
            bool profSaved = SDDatabase::saveEmployeeProfile(emp.uid, emp);
            bool nfcSaved  = SDDatabase::saveNfcMapping(cardIdentifier, emp.uid);
            Serial.printf("[STEP-3] SD cache: saveProfile=%s  saveNfcMapping=%s\n",
                profSaved ? "OK" : "FAIL", nfcSaved ? "OK" : "FAIL");
            Serial.flush();
        } else {
            Serial.printf("[STEP-3] SKIP SD cache: ready=%s uid.len=%d\n",
                SDDatabase::isReady() ? "YES" : "NO", (int)emp.uid.length());
            Serial.flush();
        }
    }

    // STEP 4: ensure photo is cached
    Serial.println("[STEP-4] --- PHOTO LOOKUP ---");
    Serial.println("[STEP-4] emp.uid='" + emp.uid + "'");
    Serial.printf("[STEP-4] SD ready=%s  uid.len=%d\n",
        SDDatabase::isReady() ? "YES" : "NO", (int)emp.uid.length());
    Serial.flush();
    String photoPath = "";
    if (SDDatabase::isReady() && emp.uid.length() > 0) {
        bool photoExists = SDDatabase::hasPhoto(emp.uid);
        Serial.printf("[STEP-4] SDDatabase::hasPhoto('%s') = %s\n",
            emp.uid.c_str(), photoExists ? "YES" : "NO");
        Serial.flush();
        if (photoExists) {
            String candidatePath = SDDatabase::photoPath(emp.uid);
            Serial.println("[STEP-4] Candidate path: " + candidatePath);
            // Double-check actual file presence and size
            if (SD_MMC.exists(candidatePath)) {
                File pf = SD_MMC.open(candidatePath, FILE_READ);
                size_t psz = pf ? pf.size() : 0;
                if (pf) pf.close();
                Serial.printf("[STEP-4] File exists on SD, size=%u bytes\n", (unsigned)psz);
                if (psz >= 100) {
                    photoPath = candidatePath;
                    Serial.println("[STEP-4] Using cached photo: " + photoPath);
                } else {
                    Serial.println("[STEP-4] WARNING: File too small (" + String(psz) + " B) - will re-download");
                }
            } else {
                Serial.println("[STEP-4] WARNING: hasPhoto=true but SD_MMC.exists=false! Stale index.");
            }
            Serial.flush();
        }

        if (photoPath.length() == 0) {
            Serial.println("[STEP-4] No valid cached photo - attempting download...");
            Serial.printf("[STEP-4] WiFi: %s  profilePicture: '%s'\n",
                wifiConfig.isConnected() ? "YES" : "NO",
                emp.profilePicture.c_str());
            Serial.flush();
            if (wifiConfig.isConnected()) {
                photoPath = downloadAndCachePhoto(emp);
                Serial.println("[STEP-4] downloadAndCachePhoto returned: '" + photoPath + "'");
                if (photoPath.length() == 0) {
                    Serial.println("[STEP-4] FAIL: Could not download photo - will use initials fallback");
                }
            } else {
                Serial.println("[STEP-4] SKIP download: no WiFi - using initials fallback");
            }
            Serial.flush();
        }
    } else {
        Serial.println("[STEP-4] SKIP: SD not ready or emp.uid empty - using initials fallback");
        Serial.flush();
    }
    Serial.println("[STEP-4] Final photoPath: '" + photoPath + "'");
    Serial.println("[STEP-4] Will show: " + String(photoPath.length() ? "JPEG photo" : "INITIALS AVATAR"));
    Serial.flush();

    // STEP 5: resolve clock type
    bool alreadyCheckedIn = SDDatabase::isReady() && SDDatabase::hasCheckedInToday(emp.uid);
    String clockType = alreadyCheckedIn ? "check-out" : "check-in";
    emp.clockType = clockType;
    lastClockType = clockType;
    lastEmployee  = emp;
    Serial.printf("[STEP-5] hasCheckedInToday=%s  clockType=%s\n",
        alreadyCheckedIn ? "YES" : "NO", clockType.c_str());
    Serial.flush();

    // STEP 6: show profile card
    Serial.println("[STEP-6] --- CALLING showEmployeeProfile ---");
    Serial.println("[STEP-6] emp.uid='" + emp.uid + "'");
    Serial.println("[STEP-6] emp.fullName='" + emp.fullName + "'");
    Serial.println("[STEP-6] emp.idNumber='" + emp.idNumber + "'");
    Serial.println("[STEP-6] emp.position='" + emp.position + "'");
    Serial.println("[STEP-6] emp.department='" + emp.department + "'");
    Serial.println("[STEP-6] emp.clockType='" + emp.clockType + "'");
    Serial.println("[STEP-6] photoPath='" + photoPath + "'");
    Serial.printf("[STEP-6] empDisplay ptr valid: %s\n", empDisplay ? "YES" : "NULL - CRASH!");
    Serial.printf("[STEP-6] Heap before render: %u  PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.flush();
    empDisplay->showEmployeeProfile(emp, photoPath);
    Serial.println("[STEP-6] showEmployeeProfile() RETURNED OK");
    Serial.println("================================================");
    Serial.flush();
    enterState(STATE_NFC_PROFILE);
}

// ─── setup ───────────────────────────────────────────────────────────────────
void setup() {
    // ── USB-CDC Serial init ───────────────────────────────────────────────────
    Serial.begin(115200);
    {
        unsigned long t0 = millis();
        while (!Serial && (millis() - t0 < 5000)) delay(10);
    }
    delay(200);

    Serial.println();
    Serial.println("========================================");
    Serial.println("[Boot] JJC Attendance System starting...");
    Serial.printf("[Boot] Chip: %s  cores: %d  freq: %d MHz\n",
                   ESP.getChipModel(), ESP.getChipCores(), getCpuFrequencyMhz());
    Serial.printf("[Boot] Free heap: %u B  PSRAM: %u B\n",
                   ESP.getFreeHeap(), ESP.getPsramSize());
    Serial.println("========================================");
    Serial.flush();

    // ── Step 1: TFT ────────────────────────────────────────────────────────
    Serial.println("[Boot] Step 1/6: TFT init...");
    if (!TFTDisplayManager::init(0)) {
        Serial.println("[Boot] ❌ TFT init FAILED — halting");
        while (true) delay(2000);
    }
    Serial.println("[Boot] ✅ TFT OK");
    dashboardInit();
    showLoadingAnimation(0, "Initializing...");
    delay(200);

    empDisplay = new EmployeeProfileDisplay();
    showLoadingAnimation(15, "Display OK");
    Serial.println("[Boot] ✅ EmployeeProfileDisplay OK");

    // ── Step 2: SD card ──────────────────────────────────────────────────────
    Serial.println("[Boot] Step 2/6: SD card init...");
    showLoadingAnimation(20, "Mounting SD...");
    if (SDDatabase::begin()) {
        showLoadingAnimation(35, "SD Ready");
        Serial.println("[Boot] ✅ SD OK");
        SDDatabase::printInfo();
    } else {
        showLoadingAnimation(35, "SD FAILED — running without local log");
        Serial.println("[Boot] ⚠️  SD FAILED — attendance will not be cached locally");
    }
    delay(200);

    // ── Step 3: NFC ──────────────────────────────────────────────────────────
    Serial.println("[Boot] Step 3/6: NFC init...");
    showLoadingAnimation(40, "NFC Init...");
    if (!nfcInit()) {
        showLoadingAnimation(100, "NFC FAILED!");
        Serial.println("[Boot] ❌ NFC FAILED — halting");
        delay(1000); while (true) delay(2000);
    }
    showLoadingAnimation(60, "NFC Ready");
    Serial.println("[Boot] ✅ NFC OK");
    delay(200);

    // ── Step 4: WiFi ─────────────────────────────────────────────────────────
    Serial.println("[Boot] Step 4/6: WiFi...");
    showLoadingAnimation(65, "WiFi connecting...");
    wifiConfig.begin();
    if (wifiConfig.isConnected()) {
        showLoadingAnimation(80, (String("WiFi: ") + wifiConfig.getSSID()).c_str());
        Serial.println("[Boot] ✅ WiFi connected: " + wifiConfig.getSSID() +
                       "  IP=" + wifiConfig.getIPAddress());
        Serial.flush();
        syncNTPTime(); // ← NEW: Sync time right away on boot
    } else {
        showLoadingAnimation(80, "WiFi: AP mode");
        Serial.println("[Boot] ⚠️  WiFi not connected — AP mode, will retry background");
    }
    delay(200);

    // ── Step 5: Web portal ───────────────────────────────────────────────────
    Serial.println("[Boot] Step 5/6: Web portal...");
    showLoadingAnimation(85, "Web Portal...");
    wifiManager.init(deviceId, &wifiConfig);
    showLoadingAnimation(95, "Portal Ready");
    Serial.println("[Boot] ✅ Web portal OK");
    delay(200);

    showLoadingAnimation(100, "System Ready!");
    delay(400);

    // Ready splash
    TFT_eSPI* t = dashboardGetTFT();
    t->fillScreen(TFTColors::BLACK);
    t->setTextColor(TFTColors::GREEN); t->setTextDatum(MC_DATUM);
    t->drawString("READY!", SCREEN_W / 2, SCREEN_H / 2, 4);
    t->setTextDatum(TL_DATUM);
    delay(500);

    drawStaticUI();
    updateStatusDots(wifiConfig.isConnected(), SDDatabase::isReady(), true);
    updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                          max(0, SDDatabase::countTodayCheckOuts()));
    enterState(STATE_DASHBOARD);

    // ── Trigger initial sync immediately if WiFi is already up ──────────────
    Serial.flush();
    if (wifiConfig.isConnected()) {
        triggerInitialSync();
    }

    Serial.println("[Ready] NFC scanning active");
    Serial.printf("[Ready] Web portal: http://%s:8080\n",
                  wifiConfig.isConnected()
                  ? wifiConfig.getIPAddress().c_str()
                  : WiFi.softAPIP().toString().c_str());
}

// ─── loop ────────────────────────────────────────────────────────────────────
void loop() {
    static unsigned long lastClock      = 0;
    static unsigned long lastNFCPoll    = 0;
    static unsigned long lastStats      = 0;
    static unsigned long lastWifiRetry  = 0;
    static unsigned long lastSync       = 0;    // attendance offline flush
    static unsigned long lastSocketPoll = 0;    // employee change detection
    static bool          wasConnected   = false;
    static uint8_t       tick           = 0;
    unsigned long now = millis();

    wifiConfig.handleClient();
    wifiManager.handleClient();

    // ── Detect WiFi connect transition ────────────────────────────────────────
    bool isConnected = wifiConfig.isConnected();
    if (isConnected && !wasConnected) {
        wasConnected = true;
        Serial.println("[WiFi] Connected → triggering initial sync and time update");
        triggerInitialSync();
        syncNTPTime(); // ← NEW: Resync time upon fresh connection
        drawStaticUI();
        updateStatusDots(true, SDDatabase::isReady(), true);
    }
    if (!isConnected && wasConnected) {
        wasConnected = false;
        updateStatusDots(false, SDDatabase::isReady(), true);
    }

    // ── Silent WiFi background reconnect ─────────────────────────────────────
    if (!isConnected && (now - lastWifiRetry >= WIFI_RETRY_MS)) {
        lastWifiRetry = now;
        Serial.println("[WiFi] Retrying connection...");
        WiFi.reconnect();
        updateStatusDots(wifiConfig.isConnected(), SDDatabase::isReady(), true);
    }

    // ── Socket poll: employee changes from server ─────────────────────────────
    // Only runs in dashboard state so it doesn't interrupt NFC processing.
    if (currentState == STATE_DASHBOARD &&
        isConnected &&
        (now - lastSocketPoll >= SOCKET_POLL_MS)) {
        lastSocketPoll = now;
        int changes = EmployeeSync::pollChanges(attService, String(SERVER_URL));
        if (changes > 0) {
            Serial.printf("[Main] %d employee change(s) applied from server\n", changes);
            // Refresh stats in case attendances were affected
            updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                                  max(0, SDDatabase::countTodayCheckOuts()));
        }
    }

    // ── Flush offline attendance queue ────────────────────────────────────────
    if (currentState == STATE_DASHBOARD &&
        (now - lastSync >= SYNC_INTERVAL_MS)) {
        lastSync = now;
        if (pendingCount > 0) flushPending();
    }

    // ── State machine ─────────────────────────────────────────────────────────
    switch (currentState) {

        case STATE_NFC_PROFILE:
            if (stateElapsed() >= NFC_CARD_DISPLAY_MS) {
                String ts = clockStr();
                SDDatabase::logAttendance(ts, lastNFCUid, lastEmployee,
                                          lastClockType, deviceId);
                if (wifiConfig.isConnected()) {
                    bool ok = attService.recordAttendance(
                        lastEmployee.uid, lastNFCUid, deviceId, lastClockType);
                    if (!ok) enqueuePending(lastEmployee.uid, lastNFCUid,
                                            lastClockType, ts);
                } else {
                    enqueuePending(lastEmployee.uid, lastNFCUid, lastClockType, ts);
                }
                empDisplay->showSuccess(lastEmployee.fullName);
                enterState(STATE_NFC_SUCCESS);
            }
            break;

        case STATE_NFC_SUCCESS:
        case STATE_NFC_ERROR:
            if (stateElapsed() >= NFC_STATUS_DISPLAY_MS) {
                enterState(STATE_DASHBOARD);
                drawStaticUI();
                updateStatusDots(wifiConfig.isConnected(),
                                 SDDatabase::isReady(), true);
                updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                                      max(0, SDDatabase::countTodayCheckOuts()));
                if (lastEmployee.hasData) {
                    char ts[6]; snprintf(ts, sizeof(ts), "%02d:%02d", clkH, clkM);
                    updateLastScan(lastEmployee.fullName, lastClockType, String(ts));
                }
            }
            break;

        default: break;
    }

    // ── Clock ─────────────────────────────────────────────────────────────────
    if (currentState == STATE_DASHBOARD && now - lastClock >= CLOCK_UPDATE_MS) {
        lastClock = now; tick++;
        tickClock(); pulseStatus(tick % 2);
        updateClock(clkH, clkM, clkS);
        if (tick % 60 == 0) updateDate(buildDateStr());
        
        // Let's resync the clock automatically once an hour (3600 seconds) 
        // to prevent time drift.
        if (tick % 3600 == 0 && wifiConfig.isConnected()) {
            syncNTPTime();
        }
    }

    // ── Stats ─────────────────────────────────────────────────────────────────
    if (currentState == STATE_DASHBOARD && now - lastStats >= STATS_REFRESH_MS) {
        lastStats = now;
        updateAttendanceStats(max(0, SDDatabase::countTodayCheckIns()),
                              max(0, SDDatabase::countTodayCheckOuts()));
    }

    // ── NFC poll ──────────────────────────────────────────────────────────────
    if (currentState == STATE_DASHBOARD && now - lastNFCPoll >= NFC_POLL_INTERVAL_MS) {
        lastNFCPoll = now;
        uint8_t uid[7] = {0}; uint8_t uidLen = 0;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 100)) {
            nfcProcessCard(uid, uidLen);
            String cardId = (nfcData.length() > 0) ? nfcData : nfcUID;
            Serial.println("[NFC] UID=" + nfcUID + " NDEF=" + nfcData + " → " + cardId);
            handleNFCDetected(cardId);
        }
    }
}