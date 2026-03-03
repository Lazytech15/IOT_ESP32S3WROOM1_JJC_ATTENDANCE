// ══════════════════════════════════════════════════════════════════════════════
// employee_sync.h  (Terminal UI Edition)
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
//   4. Terminal UI Engine added: Replaces the graphical progress bar with 
//      a live, scrolling Linux-style terminal boot sequence.
//   5. UI Overflow Fix: Shortened status codes (e.g., "SAVE:") and added dynamic
//      pixel-width calculation to fit long names without the "..." truncation.
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

        _showDownloadHeader("FULL_DB_PULL");
        
        // Changed to short terminal-style prefix to save screen space
        _updateProgress(5, "INIT_DB...", "", 0, 0);

        int saved = 0, skipped = 0, total = 0;

        // Collect UIDs in memory
        const int MAX_Q = 500;
        String* photoQueue = nullptr;
        int     photoQueueLen = 0;

        photoQueue = new String[MAX_Q];
        if (photoQueue) {
            Serial.println("[Sync] Photo queue allocated");
        } else {
            Serial.println("[Sync] ERROR: Photo queue alloc FAILED");
        }
        Serial.flush();

        Serial.println("[Sync] Calling fetchAllEmployeesEach...");
        Serial.flush();

        int fetched = svc.fetchAllEmployeesEach([&](JsonObject empJson) {
            total++;

            if (total <= 3) {
                String dbg;
                serializeJson(empJson, dbg);
                Serial.println("[Sync] emp#" + String(total) + ": " +
                               dbg.substring(0, min(300, (int)dbg.length())));
                Serial.flush();
            }

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
            
            // Shortened to "SAVE:" to give the name maximum screen width
            _updateProgress(pct, "SAVE:", emp.fullName, total, 0);

            if (!SDDatabase::hasEmployeeProfile(emp.uid)) {
                SDDatabase::saveEmployeeProfile(emp.uid, emp);
                saved++;
            } else {
                saved++;  
            }

            String nfc = _jsonStr(empJson, "nfcAccess", "nfc_access");
            if (nfc.length() > 0) SDDatabase::saveNfcMapping(nfc, emp.uid);

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
            } else {
                msg = "Fetched " + String(fetched) + " but all skipped";
            }
            delete[] photoQueue; photoQueue = nullptr;
            _showError(msg.c_str());
            return 0;
        }

        // ── Phase 2: Stream photos directly to SD ──────────────────────────────
        Serial.println("\n[Photo] ══════════════════════════════════════════════");
        Serial.println("[Photo] PHASE 2: Streaming photos to SD");
        Serial.printf( "[Photo] Total queued: %d  heap=%u  psram=%u\n",
                       photoQueueLen, ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.println("[Photo] ══════════════════════════════════════════════");
        Serial.flush();

        if (!SD_MMC.exists("/photos")) SD_MMC.mkdir("/photos");

        _updateProgress(80, "INIT_MEDIA...", "", 0, photoQueueLen);

        const size_t CHUNK  = 4096;
        uint8_t* chunk  = nullptr;
        if (psramFound()) chunk = (uint8_t*)ps_malloc(CHUNK);
        if (!chunk)        chunk = (uint8_t*)malloc(CHUNK);

        int photos   = 0;
        int ph_skip  = 0;
        int ph_fail  = 0;
        int ph_total = photoQueueLen;

        File photoLog = SD_MMC.open("/employees/photo_results.csv", FILE_WRITE);
        if (photoLog) photoLog.println("uid,result,bytes,http_code,error");

        for (int qi = 0; qi < ph_total && chunk; qi++) {
            String uid = photoQueue[qi];
            if (uid.length() == 0) { ph_skip++; continue; }

            String photoPath = "/photos/" + uid + ".jpg";
            int    pct       = 80 + (int)(18.0f * (qi + 1) / max(ph_total, 1));

            if (SDDatabase::hasPhoto(uid)) {
                File vf = SD_MMC.open(photoPath, FILE_READ);
                size_t sz = vf ? vf.size() : 0;
                if (vf) vf.close();
                if (sz > 0) {
                    if (photoLog) photoLog.println(uid + ",exists," + String(sz) + ",0,");
                    ph_skip++;
                    continue;
                }
                SD_MMC.remove(photoPath);
            }

            String url = svc.getServerURL() + "/api/profile/" + uid;
            
            // Shortened to "IMG:" to leave room
            _updateProgress(pct, "IMG:", uid, qi+1, ph_total);

            HTTPClient ph;
            ph.setTimeout(25000);
            ph.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            ph.begin(url);
            ph.addHeader("X-Client-Type", "ESP32");
            const char* hkeys[] = {"Content-Type", "Content-Length"};
            ph.collectHeaders(hkeys, 2);
            int httpCode = ph.GET();

            String ct      = ph.header("Content-Type");
            int    imgSize = ph.getSize();

            if (httpCode != 200) {
                ph.end();
                if (photoLog) photoLog.println(uid + ",http_fail,0," + String(httpCode) + ",HTTP_" + String(httpCode));
                ph_fail++;
                delay(100); yield(); continue;
            }

            if (ct.length() > 0 && ct.indexOf("image") < 0 && ct.indexOf("jpeg") < 0) {
                ph.end();
                if (photoLog) photoLog.println(uid + ",bad_content_type,0,200,CT=" + ct);
                ph_fail++;
                delay(100); yield(); continue;
            }

            if (SD_MMC.exists(photoPath)) SD_MMC.remove(photoPath);
            File f = SD_MMC.open(photoPath, FILE_WRITE);
            if (!f) {
                ph.end();
                if (photoLog) photoLog.println(uid + ",sd_open_fail,0,200,cannot_open");
                ph_fail++;
                delay(100); yield(); continue;
            }

            WiFiClient* stream  = ph.getStreamPtr();
            size_t        written = 0;
            bool          isImg   = false;  
            bool          aborted = false;
            unsigned long t0      = millis();

            while (true) {
                if (millis() - t0 > 25000) { aborted = true; break; }

                int av = stream ? stream->available() : 0;
                if (av <= 0) {
                    if (imgSize > 0 && (int)written >= imgSize) break;
                    if (!ph.connected() && av <= 0) break;
                    delay(5); yield(); continue;
                }

                size_t toRead = min((size_t)av, CHUNK);
                size_t rd     = stream->readBytes(chunk, toRead);
                if (rd == 0) { delay(2); yield(); continue; }

                if (written == 0 && rd >= 4) {
                    if (chunk[0]==0xFF && chunk[1]==0xD8 && chunk[2]==0xFF)      isImg = true;
                    else if (chunk[0]==0x89 && chunk[1]==0x50 && chunk[2]==0x4E) isImg = true;
                    else if (chunk[0]==0x47 && chunk[1]==0x49 && chunk[2]==0x46) isImg = true;
                    else if (rd>=12 && chunk[8]==0x57 && chunk[9]==0x45)         isImg = true;

                    if (!isImg) { aborted = true; break; }
                }

                size_t w = f.write(chunk, rd);
                if (w != rd) { aborted = true; break; }
                
                written += w;
                t0 = millis();  

                if (imgSize > 0 && (int)written >= imgSize) break;
                yield();
            }

            f.flush();
            f.close();
            ph.end();

            if (aborted || !isImg || written == 0) {
                SD_MMC.remove(photoPath);
                if (photoLog) photoLog.println(uid + "," + String(aborted ? "aborted" : (!isImg ? "bad_magic" : "zero_bytes")) + ",0,200,stream_fail");
                ph_fail++;
            } else {
                File vf = SD_MMC.open(photoPath, FILE_READ);
                size_t onDisk = vf ? vf.size() : 0;
                if (vf) vf.close();

                if (onDisk == written && onDisk > 0) {
                    photos++;
                    if (photoLog) photoLog.println(uid + ",saved," + String(onDisk) + ",200,");
                } else {
                    SD_MMC.remove(photoPath);
                    ph_fail++;
                    if (photoLog) photoLog.println(uid + ",verify_fail," + String(written) + ",200,onDisk=" + String(onDisk));
                }
            }
            delay(80); yield();
        } 

        if (chunk) { free(chunk); chunk = nullptr; }
        if (photoLog) { photoLog.flush(); photoLog.close(); }
        
        delete[] photoQueue;
        photoQueue = nullptr;

        // ── Save metadata ──────────────────────────────────────────────────────
        SyncMeta meta;
        meta.lastFullSyncMs = millis();
        meta.lastSocketPoll = 0.0;
        meta.totalCached    = saved;
        _saveMeta(meta);

        _updateProgress(100, "SYNC_OK", "", total, total);
        delay(1000);

        Serial.printf("[Sync] ===== FULL SYNC DONE: %d profiles, %d photos =====\n", saved, photos);
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

    // ── TERMINAL UI VARIABLES (C++17 inline statics) ──────────────────────────
    inline static String _syncLogs[12];
    inline static uint16_t _syncAddrs[12];
    inline static int _syncLogCount = 0;
    inline static int _lastLogPct = -10;
    inline static String _lastStatusStr = "";
    inline static unsigned long _lastDraw = 0;

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
        _fetchAndCacheEmployee(svc, empId);
    }

    static void _handleEmployeeUpdated(AttendanceHTTPService& svc, JsonObject data) {
        String empId = _jsonStrObj(data, "id", "employee_id");
        if (empId.length() == 0) return;
        _fetchAndCacheEmployee(svc, empId);
        uint8_t* buf = nullptr; int len = 0;
        if (svc.downloadProfileImage(empId, &buf, &len, "")) {
            SDDatabase::savePhoto(empId, buf, (size_t)len);
            free(buf);
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

    static void _fetchAndCacheEmployee(AttendanceHTTPService& svc, const String& empId) {
        String url = String(svc.getServerURL()) + "/api/employees?employeeUid=" + empId + "&limit=1&includeAllStatuses=true";
        HTTPClient http;
        http.setTimeout(10000);
        http.begin(url);
        http.addHeader("X-Client-Type", "ESP32");
        int code = http.GET();
        if (code != 200 && code != 403) { http.end(); return; }
        String body = http.getString();
        http.end();
        if (body.length() == 0) return;

        DynamicJsonDocument doc(8192);
        if (!svc.decryptBody(body, doc)) return;

        EmployeeProfile emp;
        JsonVariant empVar;
        if (doc.containsKey("employees") && doc["employees"].as<JsonArray>().size() > 0) {
            empVar = doc["employees"][0];
        } else if (doc.containsKey("data") && doc["data"].is<JsonObject>() && doc["data"]["employees"].as<JsonArray>().size() > 0) {
            empVar = doc["data"]["employees"][0];
        } else if (doc.containsKey("employee")) {
            empVar = doc["employee"];
        } else return;

        JsonObject e = empVar.as<JsonObject>();
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
        if (SD_MMC.exists(profPath))  SD_MMC.remove(profPath); 
        if (SD_MMC.exists(photoPath)) SD_MMC.remove(photoPath); 
    }

    // ── TERMINAL UI ───────────────────────────────────────────────────────────
    static void _showDownloadHeader(const char* title) {
        TFT_eSPI* tft = TFTDisplayManager::getTFT();
        if (!tft) return;
        
        // Reset state for new sync session
        _syncLogCount = 0;
        _lastLogPct = -10;
        _lastStatusStr = "";
        _lastDraw = 0;

        tft->fillScreen(TFTColors::BLACK);
        tft->setTextWrap(false); // REQUIRED: Stop TFT from auto-wrapping text to the next line
        
        tft->setTextColor(TFTColors::WHITE, TFTColors::BLACK);
        tft->setTextDatum(TL_DATUM);
        tft->drawString("JJC-OS v4.2.0-esp32s3-wroom", 4, 4, 1);
        tft->drawString("Daemon: SYSTEM_SYNC_WORKER", 4, 14, 1);
        
        char buf[64];
        snprintf(buf, sizeof(buf), "Task: %s", title);
        tft->drawString(buf, 4, 24, 1);
        
        tft->drawFastHLine(0, 36, SCREEN_WIDTH, TFTColors::BORDER_DIM);
    }

    static void _updateProgress(int pct, const char* status, const String& detail, int cur, int total) {
        TFT_eSPI* tft = TFTDisplayManager::getTFT();
        if (!tft) return;
        
        pct = constrain(pct, 0, 100);

        bool statusChanged = (String(status) != _lastStatusStr);
        
        // Throttling to max ~15 FPS to avoid blocking the network stream
        if (millis() - _lastDraw < 60 && !statusChanged && pct != 100) {
            return; 
        }
        _lastDraw = millis();
        _lastStatusStr = String(status);

        String line = String(status);
        if (detail.length() > 0) line += " " + detail;
        
        // FIX: Dynamically measure text width to fit exactly to the edge of the screen.
        // Start X is 55. Screen width is usually 240. We leave a 2 pixel margin.
        int maxTextWidth = tft->width() - 58; 
        
        // Trim characters from the end one by one until it fits the screen exactly
        while (line.length() > 0 && tft->textWidth(line, 2) > maxTextWidth) {
            line.remove(line.length() - 1);
        }

        bool newLine = statusChanged || (pct - _lastLogPct >= 10) || (pct == 100);

        if (newLine || _syncLogCount == 0) {
            if (_syncLogCount < 12) {
                _syncLogs[_syncLogCount] = line;
                _syncAddrs[_syncLogCount] = 0x9000 + (pct * 0x10);
                _syncLogCount++;
            } else {
                for (int i = 0; i < 11; i++) {
                    _syncLogs[i] = _syncLogs[i+1];
                    _syncAddrs[i] = _syncAddrs[i+1];
                }
                _syncLogs[11] = line;
                _syncAddrs[11] = 0x9000 + (pct * 0x10);
            }
            _lastLogPct = pct;
        } else {
            // Update the last line in place
            _syncLogs[_syncLogCount - 1] = line;
        }

        // Clear logging and prompt area to prevent text overlap
        tft->fillRect(0, 40, tft->width(), tft->height() - 40, TFTColors::BLACK);

        int y = 42;
        for (int i = 0; i < _syncLogCount; i++) {
            char prefix[16];
            snprintf(prefix, sizeof(prefix), "[%04X] ", _syncAddrs[i]);
            
            uint16_t col = TFTColors::ACCENT_CYAN;
            
            // Adjusted color logic: INIT is dim, SAVE/IMG defaults to bright Green
            if (_syncLogs[i].indexOf("FAIL") >= 0 || _syncLogs[i].indexOf("ERROR") >= 0) {
                col = TFTColors::RED;
            } else if (_syncLogs[i].indexOf("INIT") >= 0 || _syncLogs[i].indexOf("...") > 0) {
                col = TFTColors::TEXT_DIM;
            } else {
                col = TFTColors::SUCCESS;
            }

            tft->setTextColor(TFTColors::TEXT_DIM, TFTColors::BLACK);
            tft->drawString(prefix, 4, y, 2);
            
            tft->setTextColor(col, TFTColors::BLACK);
            tft->drawString(_syncLogs[i], 55, y, 2);
            y += 18;
        }

        // ── Command Prompt Area ──
        int promptY = 275;
        tft->drawFastHLine(0, promptY-4, tft->width(), TFTColors::BORDER_DIM);
        tft->setTextColor(TFTColors::SUCCESS, TFTColors::BLACK);
        tft->drawString("root@jjc-sys:/sync#", 4, promptY, 2);
        
        char pBuf[32];
        if (total > 0) {
            snprintf(pBuf, sizeof(pBuf), "rcv %d%% (%d/%d)", pct, cur, total);
        } else {
            snprintf(pBuf, sizeof(pBuf), "rcv %d%%", pct);
        }
        tft->setTextColor(TFTColors::WHITE, TFTColors::BLACK);
        tft->drawString(pBuf, 4, promptY + 20, 2);

        // Blinking cursor block
        int textW = tft->textWidth(pBuf, 2);
        if ((millis() / 300) % 2 == 0) {
            tft->fillRect(4 + textW + 4, promptY + 20, 8, 14, TFTColors::SUCCESS);
        }
    }

    static void _showError(const char* msg) {
        Serial.println("[Sync] ❌ ERROR: " + String(msg));
        Serial.flush();
        TFT_eSPI* tft = TFTDisplayManager::getTFT();
        if (!tft) return;
        
        tft->fillRect(0, 260, SCREEN_WIDTH, 60, TFTColors::BLACK);
        tft->drawFastHLine(0, 260, SCREEN_WIDTH, TFTColors::RED);
        tft->setTextDatum(TL_DATUM);
        
        tft->setTextColor(TFTColors::RED, TFTColors::BLACK);
        tft->drawString("FATAL SYNC ERROR:", 4, 268, 2);
        
        String m = String(msg);
        if (m.length() > 25) m = m.substring(0, 22) + "...";
        tft->setTextColor(TFTColors::WHITE, TFTColors::BLACK);
        tft->drawString(m, 4, 288, 2);
        
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