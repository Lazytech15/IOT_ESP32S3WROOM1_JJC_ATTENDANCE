// ══════════════════════════════════════════════════════════════════════════════
// employee_sync.h  (fixed v3)
//
// KEY FIXES vs v2:
//   1. fullSync() callback maps emp.uid from "id" (int) which is what PHP
//      formatEmployeeData() actually returns — not "uid". The old mapping
//      _jsonStr(empJson, "id", "uid") would work IF the field exists, but
//      now we also log the raw value so you can confirm it's non-zero.
//   2. emp.hasData guard logs the exact uid/fullName that caused the skip so
//      nothing disappears silently.
//   3. attendance_http_service.h fetchAllEmployeesEach now uses
//      decryptServerResponse() (v3) so the callback here receives properly
//      decrypted JsonObjects — no more silent NoMemory failures from the
//      old 8192-byte per-employee document budget.
//   4. All other fixes from v2 retained (Serial.flush, _showError 3s, etc.)
// ══════════════════════════════════════════════════════════════════════════════

#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <new>
#include "TFTDisplayManager.h"
#include "attendance_http_service.h"
#include "sd_database.h"
#include "employee_profile_display.h"

#define SYNC_META_PATH          "/employees/sync_meta.json"
#define FULL_SYNC_INTERVAL_MS   (6UL * 3600UL * 1000UL)
#define SOCKET_POLL_INTERVAL_MS 60000UL
#define MAX_EMPLOYEES_PER_FETCH 500
#define PHOTO_DOWNLOAD_TIMEOUT  12000
#define PHOTO_BATCH_YIELD_MS    20

// Progress bar geometry (portrait 240×320)
#define PROG_TITLE_Y    60
#define PROG_BAR_X      20
#define PROG_BAR_Y      120
#define PROG_BAR_W      200
#define PROG_BAR_H      18
#define PROG_STATUS_Y   150
#define PROG_DETAIL_Y   170
#define PROG_COUNT_Y    195
#define PROG_CANCEL_Y   290

class EmployeeSync {
public:

    // ── fullSyncIfNeeded ──────────────────────────────────────────────────────
    static void fullSyncIfNeeded(AttendanceHTTPService& svc, bool force = false) {
        if (!SDDatabase::isReady()) {
            Serial.println("[Sync] fullSyncIfNeeded: SD not ready — skipping");
            Serial.flush();
            return;
        }

        if (!force) {
            SyncMeta meta = _loadMeta();
            unsigned long elapsed = millis() - meta.lastFullSyncMs;
            if (meta.lastFullSyncMs > 0 && elapsed < FULL_SYNC_INTERVAL_MS) {
                Serial.printf("[Sync] Full sync skipped — %lu min ago (next in %lu min)\n",
                              elapsed / 60000UL,
                              (FULL_SYNC_INTERVAL_MS - elapsed) / 60000UL);
                Serial.printf("[Sync] Cached employees: %d\n", meta.totalCached);
                Serial.flush();
                return;
            }
        }
        fullSync(svc);
    }

    // ── fullSync ──────────────────────────────────────────────────────────────
    static int fullSync(AttendanceHTTPService& svc) {
        Serial.println("[Sync] ===== FULL EMPLOYEE SYNC START =====");
        Serial.printf("[Sync] Heap:  %u B free\n", ESP.getFreeHeap());
        Serial.printf("[Sync] PSRAM: %u B free\n", ESP.getFreePsram());
        Serial.flush();

        _showDownloadHeader("Syncing Employees");
        _updateProgress(5, "Fetching employees...", "", 0, 0);

        int saved = 0, skipped = 0, total = 0;

        // Collect UIDs in memory — do NOT hold a File handle open inside the
        // lambda. SD_MMC File objects captured by reference across HTTP
        // callbacks get corrupted on ESP32, producing 0-byte queue files.
        // We use a fixed array (PSRAM if available) for up to 500 UIDs.
        const int MAX_Q = 500;
        String* photoQueue = nullptr;
        int     photoQueueLen = 0;

        if (psramFound()) {
            photoQueue = (String*)ps_malloc(MAX_Q * sizeof(String));
            if (photoQueue) {
                for (int i = 0; i < MAX_Q; i++) ::new (&photoQueue[i]) String();
                Serial.println("[Sync] Photo queue array in PSRAM");
            }
        }
        if (!photoQueue) {
            photoQueue = new String[MAX_Q];
            Serial.println("[Sync] Photo queue array in heap");
        }
        Serial.flush();

        Serial.println("[Sync] Calling fetchAllEmployeesEach...");
        Serial.flush();

        int fetched = svc.fetchAllEmployeesEach([&](JsonObject empJson) {
            total++;

            // ── Debug: print first 3 employees to verify field names ──────────
            if (total <= 3) {
                String dbg;
                serializeJson(empJson, dbg);
                Serial.println("[Sync] emp#" + String(total) + ": " +
                               dbg.substring(0, min(300, (int)dbg.length())));
                Serial.flush();
            }

            // ── Map fields ────────────────────────────────────────────────────
            EmployeeProfile emp;
            emp.uid            = _jsonStr(empJson, "id",             "uid");
            emp.idNumber       = _jsonStr(empJson, "idNumber",       "id_number");
            emp.fullName       = _jsonStr(empJson, "fullName",       "full_name");
            emp.firstName      = _jsonStr(empJson, "firstName",      "first_name");
            emp.lastName       = _jsonStr(empJson, "lastName",       "last_name");
            emp.position       = _jsonStr(empJson, "position",       "position");
            emp.department     = _jsonStr(empJson, "department",     "department");
            emp.email          = _jsonStr(empJson, "email",          "email");
            emp.status         = _jsonStr(empJson, "status",         "status");
            emp.employmentType = _jsonStr(empJson, "employmentType", "employment_type");
            emp.profilePicture = _jsonStr(empJson, "profilePicture", "profile_picture");
            emp.accessGranted  = (emp.status == "Active");
            emp.hasData        = (emp.uid.length() > 0 && emp.uid != "0");

            if (!emp.hasData) {
                skipped++;
                Serial.printf("[Sync] SKIPPED #%d — uid='%s' fullName='%s'\n",
                              total, emp.uid.c_str(), emp.fullName.c_str());
                Serial.flush();
                return;
            }

            int pct = 5 + (int)(75.0f * saved / max(total, 1));
            _updateProgress(pct, "Caching profiles...", emp.fullName, total, 0);

            SDDatabase::saveEmployeeProfile(emp.uid, emp);
            saved++;

            String nfc = _jsonStr(empJson, "nfcAccess", "nfc_access");
            if (nfc.length() > 0) SDDatabase::saveNfcMapping(nfc, emp.uid);

            // Queue uid in memory — file write happens AFTER lambda completes
            if (photoQueueLen < MAX_Q) {
                photoQueue[photoQueueLen++] = emp.uid;
            }

            if (saved % 10 == 0) {
                Serial.printf("[Sync] saved=%d total=%d skipped=%d heap=%u\n",
                              saved, total, skipped, ESP.getFreeHeap());
                Serial.flush();
            }
            yield();
        });

        Serial.printf("[Sync] Phase 1 done: fetched=%d saved=%d skipped=%d queued=%d\n",
                      fetched, saved, skipped, photoQueueLen);
        Serial.flush();

        if (saved == 0) {
            String msg;
            if (fetched == 0) {
                msg = "No data from server (network/key/URL?)";
                Serial.println("[Sync] ❌ fetched=0 — check WiFi/URL/AES key");
            } else {
                msg = "Fetched " + String(fetched) + " but all skipped";
                Serial.println("[Sync] ❌ fetched=" + String(fetched) + " saved=0 — uid=0?");
            }
            Serial.flush();
            // Free queue memory before returning
            if (psramFound() && photoQueue) { free(photoQueue); } else { delete[] photoQueue; }
            _showError(msg.c_str());
            return 0;
        }

        // ── Phase 2: download photos ───────────────────────────────────────────
        // Ensure /photos directory exists before any file writes
        if (!SD_MMC.exists("/photos")) {
            bool created = SD_MMC.mkdir("/photos");
            Serial.printf("[Sync] mkdir /photos: %s\n", created ? "OK" : "FAILED");
            Serial.flush();
        } else {
            Serial.println("[Sync] /photos dir already exists");
            Serial.flush();
        }

        _updateProgress(80, "Downloading photos...", "", 0, 0);
        int photos = 0;
        int queueSize = photoQueueLen;

        Serial.printf("[Sync] Starting photo downloads: %d queued\n", queueSize);
        Serial.flush();

        // ── Photo result log — written to SD so you can inspect it in the browser
        File photoLog = SD_MMC.open("/employees/photo_results.csv", FILE_WRITE);
        if (photoLog) {
            photoLog.println("uid,result,bytes,error");
        }

        for (int qi = 0; qi < queueSize; qi++) {
            String uid = photoQueue[qi];
            if (uid.length() == 0) continue;

            String photoPath = "/photos/" + uid + ".jpg";

            // Skip if genuinely already on SD from a previous sync
            if (SD_MMC.exists(photoPath)) {
                File vf = SD_MMC.open(photoPath, FILE_READ);
                size_t existSz = vf ? vf.size() : 0;
                if (vf) vf.close();
                Serial.printf("[Sync] Photo exists uid=%s size=%u — skip\n", uid.c_str(), (unsigned)existSz);
                if (photoLog) photoLog.println(uid + ",exists," + String(existSz) + ",");
                Serial.flush();
                continue;
            }

            int pct = 80 + (int)(18.0f * (qi + 1) / max(queueSize, 1));
            _updateProgress(pct, "Downloading photos...", uid, qi + 1, queueSize);
            Serial.printf("[Sync] Photo %d/%d uid=%s heap=%u\n",
                          qi + 1, queueSize, uid.c_str(), ESP.getFreeHeap());
            Serial.flush();

            uint8_t* buf = nullptr; int imgLen = 0;
            bool dlOk = svc.downloadProfileImage(uid, &buf, &imgLen, "");
            if (dlOk && buf && imgLen > 0) {
                bool svOk = SDDatabase::savePhoto(uid, buf, (size_t)imgLen);
                free(buf);
                if (svOk) {
                    photos++;
                    Serial.printf("[Sync] OK photo uid=%s bytes=%d\n", uid.c_str(), imgLen);
                    if (photoLog) photoLog.println(uid + ",saved," + String(imgLen) + ",");
                } else {
                    Serial.printf("[Sync] FAIL savePhoto uid=%s bytes=%d\n", uid.c_str(), imgLen);
                    if (photoLog) photoLog.println(uid + ",save_failed," + String(imgLen) + ",savePhoto returned false");
                }
            } else {
                if (buf) free(buf);
                Serial.printf("[Sync] FAIL download uid=%s dlOk=%d bytes=%d\n",
                              uid.c_str(), (int)dlOk, imgLen);
                if (photoLog) photoLog.println(uid + ",download_failed,0,downloadProfileImage returned false");
            }
            Serial.flush();
            delay(50);   // brief pause — gives TCP stack time to fully close previous connection
            yield();
        }

        if (photoLog) {
            photoLog.flush();
            photoLog.close();
            Serial.println("[Sync] Photo results written to /employees/photo_results.csv");
            Serial.flush();
        }

        // Explicitly call String destructors before freeing PSRAM/heap array
        for (int i = 0; i < MAX_Q; i++) photoQueue[i].~String();
        if (psramFound()) { free(photoQueue); } else { delete[] photoQueue; }
        photoQueue = nullptr;

        // ── Save metadata ──────────────────────────────────────────────────────
        SyncMeta meta;
        meta.lastFullSyncMs = millis();
        meta.lastSocketPoll = 0.0;
        meta.totalCached    = saved;
        _saveMeta(meta);

        _updateProgress(100, "Sync complete!", "", total, total);
        delay(1000);

        Serial.printf("[Sync] ===== FULL SYNC DONE: %d profiles, %d photos =====\n",
                      saved, photos);
        Serial.flush();
        return saved;
    }

    // ── pollChanges ───────────────────────────────────────────────────────────
    static int pollChanges(AttendanceHTTPService& svc, const String& serverURL) {
        if (!SDDatabase::isReady()) return 0;

        SyncMeta meta = _loadMeta();
        String url = serverURL + "/api/socket.php?action=poll&since=" +
                     String(meta.lastSocketPoll, 6);
        Serial.println("[Poll] GET " + url);
        Serial.flush();

        HTTPClient http;
        http.setTimeout(8000);
        http.begin(url);
        int code = http.GET();
        if (code != 200) {
            Serial.printf("[Poll] HTTP %d — skipping\n", code);
            Serial.flush();
            http.end();
            return 0;
        }

        String body = http.getString();
        http.end();
        if (body.length() == 0) return 0;

        DynamicJsonDocument doc(8192);
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            Serial.println("[Poll] JSON parse error");
            Serial.flush();
            return 0;
        }

        double newTs = doc["timestamp"] | meta.lastSocketPoll;
        meta.lastSocketPoll = newTs;
        _saveMeta(meta);

        JsonArray events = doc["events"].as<JsonArray>();
        if (events.isNull() || events.size() == 0) return 0;

        int changes = 0;
        for (JsonObject evt : events) {
            String evtName = evt["event"] | "";
            JsonObject data = evt["data"].as<JsonObject>();
            Serial.println("[Poll] Event: " + evtName);
            Serial.flush();

            if (evtName == "employee_created") {
                _handleEmployeeCreated(svc, data); changes++;
            } else if (evtName == "employee_updated" ||
                       evtName == "employee_status_changed") {
                _handleEmployeeUpdated(svc, data); changes++;
            } else if (evtName == "employee_deleted" ||
                       evtName == "employee_bulk_deleted") {
                _handleEmployeeDeleted(data); changes++;
            }
        }

        if (changes > 0) {
            Serial.printf("[Poll] Applied %d change(s)\n", changes);
            Serial.flush();
        }
        return changes;
    }

    static unsigned long lastSyncAgeMs() {
        SyncMeta m = _loadMeta();
        if (m.lastFullSyncMs == 0) return 0;
        return millis() - m.lastFullSyncMs;
    }

    static int cachedCount() { return _loadMeta().totalCached; }

private:
    struct SyncMeta {
        unsigned long lastFullSyncMs = 0;
        double        lastSocketPoll = 0.0;
        int           totalCached    = 0;
    };

    static SyncMeta _loadMeta() {
        SyncMeta m;
        if (!SDDatabase::isReady()) return m;
        if (!SD_MMC.exists(SYNC_META_PATH)) return m;
        File f = SD_MMC.open(SYNC_META_PATH, FILE_READ);
        if (!f) return m;
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, f) == DeserializationError::Ok) {
            m.lastFullSyncMs = doc["lastFullSync"]   | (unsigned long)0;
            m.lastSocketPoll = doc["lastSocketPoll"] | 0.0;
            m.totalCached    = doc["totalCached"]    | 0;
        }
        f.close();
        return m;
    }

    static void _saveMeta(const SyncMeta& m) {
        if (!SDDatabase::isReady()) return;
        File f = SD_MMC.open(SYNC_META_PATH, FILE_WRITE);
        if (!f) return;
        DynamicJsonDocument doc(256);
        doc["lastFullSync"]   = m.lastFullSyncMs;
        doc["lastSocketPoll"] = m.lastSocketPoll;
        doc["totalCached"]    = m.totalCached;
        serializeJson(doc, f);
        f.close();
    }

    static void _handleEmployeeCreated(AttendanceHTTPService& svc, JsonObject data) {
        String empId = _jsonStrObj(data, "id", "employee_id");
        if (empId.length() == 0) return;
        Serial.println("[Poll] 👤 New employee: " + empId);
        Serial.flush();
        _fetchAndCacheEmployee(svc, empId);
    }

    static void _handleEmployeeUpdated(AttendanceHTTPService& svc, JsonObject data) {
        String empId = _jsonStrObj(data, "id", "employee_id");
        if (empId.length() == 0) return;
        Serial.println("[Poll] 🔄 Updated employee: " + empId);
        Serial.flush();
        _fetchAndCacheEmployee(svc, empId);
        uint8_t* buf = nullptr; int len = 0;
        if (svc.downloadProfileImage(empId, &buf, &len, "")) {
            SDDatabase::savePhoto(empId, buf, (size_t)len);
            free(buf);
            Serial.println("[Poll] 📷 Photo updated: " + empId);
            Serial.flush();
        }
    }

    static void _handleEmployeeDeleted(JsonObject data) {
        String empId = _jsonStrObj(data, "id", "employee_id");
        if (empId.length() > 0) _removeEmployeeFromCache(empId);
        if (data.containsKey("deleted_ids")) {
            JsonArray ids = data["deleted_ids"].as<JsonArray>();
            for (JsonVariant v : ids) {
                String id = v.as<String>();
                if (id.length() > 0) _removeEmployeeFromCache(id);
            }
        }
    }

    static void _fetchAndCacheEmployee(AttendanceHTTPService& svc,
                                        const String& empId) {
        String url = String(svc.getServerURL()) +
                     "/api/employees?employeeUid=" + empId +
                     "&limit=1&includeAllStatuses=true";
        HTTPClient http;
        http.setTimeout(10000);
        http.begin(url);
        http.addHeader("X-Client-Type", "ESP32");
        int code = http.GET();
        if (code != 200 && code != 403) {
            Serial.printf("[Poll] Employee fetch HTTP %d for id=%s\n",
                          code, empId.c_str());
            Serial.flush();
            http.end(); return;
        }
        String body = http.getString();
        http.end();
        if (body.length() == 0) return;

        DynamicJsonDocument doc(8192);
        if (!svc.decryptBody(body, doc)) return;

        EmployeeProfile emp;
        JsonVariant empVar;
        if (doc.containsKey("employees") &&
            doc["employees"].as<JsonArray>().size() > 0) {
            empVar = doc["employees"][0];
        } else if (doc.containsKey("data") &&
                   doc["data"].is<JsonObject>() &&
                   doc["data"]["employees"].as<JsonArray>().size() > 0) {
            empVar = doc["data"]["employees"][0];
        } else if (doc.containsKey("employee")) {
            empVar = doc["employee"];
        } else {
            return;
        }

        JsonObject e = empVar.as<JsonObject>();
        // PHP returns camelCase "id" as integer for formatEmployeeData
        emp.uid            = _jsonStrObj(e, "id",             "uid");
        emp.idNumber       = _jsonStrObj(e, "idNumber",       "id_number");
        emp.fullName       = _jsonStrObj(e, "fullName",       "full_name");
        emp.firstName      = _jsonStrObj(e, "firstName",      "first_name");
        emp.lastName       = _jsonStrObj(e, "lastName",       "last_name");
        emp.position       = _jsonStrObj(e, "position",       "position");
        emp.department     = _jsonStrObj(e, "department",     "department");
        emp.email          = _jsonStrObj(e, "email",          "email");
        emp.status         = _jsonStrObj(e, "status",         "status");
        emp.employmentType = _jsonStrObj(e, "employmentType", "employment_type");
        emp.profilePicture = _jsonStrObj(e, "profilePicture", "profile_picture");
        emp.accessGranted  = (emp.status == "Active");
        emp.hasData        = (emp.uid.length() > 0 && emp.uid != "0");
        if (!emp.hasData) return;

        SDDatabase::saveEmployeeProfile(emp.uid, emp);
        Serial.println("[Poll] 💾 Cached: " + emp.fullName +
                       " (uid=" + emp.uid + ")");
        Serial.flush();

        String nfcA = _jsonStrObj(e, "nfcAccess", "nfc_access");
        if (nfcA.length() > 0) SDDatabase::saveNfcMapping(nfcA, emp.uid);

        if (!SDDatabase::hasPhoto(emp.uid)) {
            uint8_t* buf = nullptr; int len = 0;
            if (svc.downloadProfileImage(emp.uid, &buf, &len, emp.profilePicture)) {
                SDDatabase::savePhoto(emp.uid, buf, (size_t)len);
                free(buf);
            }
        }
    }

    static void _removeEmployeeFromCache(const String& empId) {
        String profPath  = "/employees/" + empId + ".json";
        String photoPath = "/photos/"    + empId + ".jpg";
        if (SD_MMC.exists(profPath))  { SD_MMC.remove(profPath);  Serial.println("[Poll] 🗑️  Removed profile: " + empId); }
        if (SD_MMC.exists(photoPath)) { SD_MMC.remove(photoPath); Serial.println("[Poll] 🗑️  Removed photo:   " + empId); }
        Serial.flush();
    }

    // ── TFT progress UI ───────────────────────────────────────────────────────
    static void _showDownloadHeader(const char* title) {
        TFT_eSPI* tft = TFTDisplayManager::getTFT();
        if (!tft) return;
        tft->fillScreen(TFTColors::BG_DARK);
        tft->fillRect(0, 0, SCREEN_WIDTH, 50, TFTColors::BG_MID);
        tft->drawFastHLine(0, 50, SCREEN_WIDTH, TFTColors::BORDER_BRIGHT);
        int cx = SCREEN_WIDTH / 2;
        tft->fillCircle(cx-18, 25, 12, TFTColors::ACCENT_TEAL);
        tft->fillCircle(cx,    20, 15, TFTColors::ACCENT_TEAL);
        tft->fillCircle(cx+18, 25, 11, TFTColors::ACCENT_TEAL);
        tft->fillRect(cx-18, 25, 36, 12, TFTColors::ACCENT_TEAL);
        tft->fillTriangle(cx, 45, cx-8, 35, cx+8, 35, TFTColors::BLACK);
        tft->fillRect(cx-3, 28, 6, 10, TFTColors::BLACK);
        tft->setTextDatum(MC_DATUM);
        tft->setTextColor(TFTColors::TEXT_PRIMARY, TFTColors::BG_DARK);
        tft->drawString(title, cx, PROG_TITLE_Y, 2);
        tft->setTextDatum(TL_DATUM);
        tft->drawRoundRect(PROG_BAR_X-1, PROG_BAR_Y-1,
                           PROG_BAR_W+2, PROG_BAR_H+2, 4, TFTColors::BORDER_DIM);
    }

    static void _updateProgress(int pct, const char* status,
                                  const String& detail, int cur, int total) {
        TFT_eSPI* tft = TFTDisplayManager::getTFT();
        if (!tft) return;
        pct = constrain(pct, 0, 100);
        int filled = (int)((float)PROG_BAR_W * pct / 100.0f);

        tft->fillRoundRect(PROG_BAR_X, PROG_BAR_Y, PROG_BAR_W, PROG_BAR_H,
                           3, TFTColors::BG_LIGHT);
        if (filled > 0)
            tft->fillRoundRect(PROG_BAR_X, PROG_BAR_Y, filled, PROG_BAR_H,
                               3, TFTColors::ACCENT_TEAL);
        if (filled > 4)
            tft->drawFastHLine(PROG_BAR_X+2, PROG_BAR_Y+4,
                               filled-4, TFTColors::ACCENT_CYAN);

        char pctBuf[6]; snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
        tft->setTextDatum(MC_DATUM);
        tft->setTextColor(pct > 50 ? TFTColors::BLACK : TFTColors::TEXT_PRIMARY,
                          pct > 50 ? TFTColors::ACCENT_TEAL : TFTColors::BG_LIGHT);
        tft->drawString(pctBuf, PROG_BAR_X + PROG_BAR_W/2, PROG_BAR_Y + PROG_BAR_H/2, 1);

        tft->fillRect(0, PROG_STATUS_Y-8, SCREEN_WIDTH, 18, TFTColors::BG_DARK);
        tft->setTextColor(TFTColors::TEXT_SECONDARY, TFTColors::BG_DARK);
        tft->drawString(status, SCREEN_WIDTH/2, PROG_STATUS_Y, 1);

        tft->fillRect(0, PROG_DETAIL_Y-8, SCREEN_WIDTH, 18, TFTColors::BG_DARK);
        if (detail.length() > 0) {
            String d = detail;
            while (d.length() > 1 && tft->textWidth(d.c_str(), 1) > SCREEN_WIDTH-20)
                d.remove(d.length()-1);
            tft->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
            tft->drawString(d, SCREEN_WIDTH/2, PROG_DETAIL_Y, 1);
        }

        tft->fillRect(0, PROG_COUNT_Y-8, SCREEN_WIDTH, 18, TFTColors::BG_DARK);
        if (total > 0) {
            char countBuf[32];
            snprintf(countBuf, sizeof(countBuf), "%d / %d", cur, total);
            tft->setTextColor(TFTColors::ACCENT_YELLOW, TFTColors::BG_DARK);
            tft->drawString(countBuf, SCREEN_WIDTH/2, PROG_COUNT_Y, 2);
        }

        tft->fillRect(0, PROG_CANCEL_Y-8, SCREEN_WIDTH, 20, TFTColors::BG_DARK);
        tft->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_DARK);
        tft->drawString("Please wait...", SCREEN_WIDTH/2, PROG_CANCEL_Y, 1);
        tft->setTextDatum(TL_DATUM);
    }

    static void _showError(const char* msg) {
        Serial.println("[Sync] ❌ ERROR: " + String(msg));
        Serial.flush();
        TFT_eSPI* tft = TFTDisplayManager::getTFT();
        if (!tft) return;
        tft->fillRect(0, PROG_STATUS_Y-10, SCREEN_WIDTH, 50, TFTColors::BG_DARK);
        tft->setTextDatum(MC_DATUM);
        tft->setTextColor(TFTColors::ERROR, TFTColors::BG_DARK);
        tft->drawString(msg, SCREEN_WIDTH/2, PROG_STATUS_Y+10, 1);
        tft->setTextDatum(TL_DATUM);
        delay(3000);
    }

    // ── JSON helpers ──────────────────────────────────────────────────────────
    static String _jsonStr(JsonObject obj, const char* key1, const char* key2) {
        for (const char* key : {key1, key2}) {
            if (obj.containsKey(key) && !obj[key].isNull()) {
                JsonVariantConst v = obj[key];
                if (v.is<int>())   return String(v.as<int>());
                if (v.is<long>())  return String(v.as<long>());
                const char* s = v.as<const char*>();
                return s ? String(s) : String("");
            }
        }
        return "";
    }

    static String _jsonStrObj(JsonObject obj, const char* key1, const char* key2) {
        return _jsonStr(obj, key1, key2);
    }
};