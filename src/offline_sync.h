// ══════════════════════════════════════════════════════════════════════════════
// offline_sync.h
//
// OFFLINE-FIRST ATTENDANCE SYNC
//
// Architecture:
//   1. Every scan is ALWAYS written to SD first (guaranteed local record).
//   2. A "pending" queue file (/attendance/pending_sync.json) tracks records
//      that have NOT yet been confirmed by the server.
//   3. When WiFi is available, syncPending() POSTs each pending record to the
//      server and removes it from the queue on success.
//   4. If the server is unreachable, the record stays in the queue and will
//      be retried the next time syncPending() is called.
//
// Queue file format (/attendance/pending_sync.json):
//   [ {"ts":"HH:MM:SS","uid":"<empUid>","nfc":"<nfcUid>",
//      "event":"check-in","device":"Attendance_Display_01",
//      "name":"Full Name","dept":"Department"}, ... ]
//
// Usage in main.cpp loop():
//   // After any state change or periodically:
//   if (wifiConfig.isConnected() && OfflineSync::hasPending())
//       OfflineSync::syncPending(attService, deviceId);
//
// Employee cache fetch:
//   OfflineSync::prefetchAllEmployees(attService) — call once on startup
//   when WiFi is available, downloads all employees + photos to SD.
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include "sd_database.h"
#include "employee_profile_display.h"
#include "attendance_http_service.h"

#define PENDING_FILE   "/attendance/pending_sync.json"
#define EMPLOYEES_FILE "/employees/employee_index.json"
#define MAX_PENDING    200   // max records kept in queue

// ══════════════════════════════════════════════════════════════════════════════
class OfflineSync {
public:

    // ── enqueuePending ────────────────────────────────────────────────────────
    // Add a record to the pending-sync queue.
    // Always call this AFTER logAttendance() to SD.
    static bool enqueuePending(const String& timestamp,
                               const String& empUid,
                               const String& nfcUid,
                               const String& eventType,
                               const String& deviceId,
                               const String& empName   = "",
                               const String& empDept   = "") {
        if (!SDDatabase::isReady()) return false;

        // Read existing queue
        DynamicJsonDocument doc(MAX_PENDING * 180);
        _loadQueue(doc);

        JsonArray arr = doc.as<JsonArray>();

        // Cap the queue to avoid unbounded growth
        while ((int)arr.size() >= MAX_PENDING) {
            arr.remove(0);
        }

        JsonObject entry = arr.createNestedObject();
        entry["ts"]     = timestamp;
        entry["uid"]    = empUid;
        entry["nfc"]    = nfcUid;
        entry["event"]  = eventType;
        entry["device"] = deviceId;
        entry["name"]   = empName;
        entry["dept"]   = empDept;

        return _saveQueue(doc);
    }

    // ── hasPending ────────────────────────────────────────────────────────────
    static bool hasPending() {
        if (!SDDatabase::isReady()) return false;
        if (!SD_MMC.exists(PENDING_FILE)) return false;
        File f = SD_MMC.open(PENDING_FILE, FILE_READ);
        if (!f) return false;
        bool nonEmpty = (f.size() > 5);  // "[]" or "[ ]" = empty
        f.close();
        return nonEmpty;
    }

    // ── pendingCount ──────────────────────────────────────────────────────────
    static int pendingCount() {
        if (!SDDatabase::isReady()) return 0;
        DynamicJsonDocument doc(MAX_PENDING * 180);
        _loadQueue(doc);
        return doc.as<JsonArray>().size();
    }

    // ── syncPending ───────────────────────────────────────────────────────────
    // Tries to send each pending record to the server.
    // Removes confirmed records from the queue.
    // Returns number of records successfully synced.
    static int syncPending(AttendanceHTTPService& attService,
                           const String& deviceId) {
        if (!SDDatabase::isReady()) return 0;
        if (!hasPending()) return 0;

        Serial.println("[Sync] Starting sync of pending attendance records...");

        DynamicJsonDocument doc(MAX_PENDING * 180);
        _loadQueue(doc);
        JsonArray arr = doc.as<JsonArray>();

        int synced  = 0;
        int total   = arr.size();
        int removed = 0;

        // We iterate and build a new array of FAILED records
        DynamicJsonDocument newDoc(MAX_PENDING * 180);
        JsonArray newArr = newDoc.to<JsonArray>();

        for (JsonObject entry : arr) {
            String uid    = entry["uid"]    | "";
            String nfc    = entry["nfc"]    | "";
            String event  = entry["event"]  | "check-in";
            String dev    = entry["device"] | deviceId;

            Serial.printf("[Sync] → %s  event=%s\n", uid.c_str(), event.c_str());

            bool ok = attService.recordAttendance(uid, nfc, dev, event);

            if (ok) {
                synced++;
                removed++;
                Serial.printf("[Sync] ✅ Synced (%d/%d)\n", synced, total);
            } else {
                // Keep in queue for next retry
                JsonObject keep = newArr.createNestedObject();
                keep["ts"]     = entry["ts"];
                keep["uid"]    = uid;
                keep["nfc"]    = nfc;
                keep["event"]  = event;
                keep["device"] = dev;
                keep["name"]   = entry["name"];
                keep["dept"]   = entry["dept"];
                Serial.println("[Sync] ⏳ Will retry later");
            }

            yield();  // keep WDT happy
        }

        _saveQueue(newDoc);

        Serial.printf("[Sync] Done: %d/%d synced, %d remain\n",
                      synced, total, (int)newArr.size());
        return synced;
    }

    // ── prefetchAllEmployees ──────────────────────────────────────────────────
    // Downloads all employees from /api/employees on the server and caches:
    //   • Employee JSON profile  → /employees/<uid>.json
    //   • Profile photo JPEG     → /photos/<uid>.jpg
    //
    // Call once on startup (after WiFi connects) to prime the local cache.
    // If the SD already has profiles they are skipped (incremental update).
    //
    // Returns number of employees fetched/updated.
    static int prefetchAllEmployees(AttendanceHTTPService& attService) {
        Serial.println("[Prefetch] Starting employee prefetch...");

        // Fetch employee list from server
        DynamicJsonDocument listDoc(16384);
        bool ok = attService.fetchAllEmployees(listDoc);
        if (!ok) {
            Serial.println("[Prefetch] ❌ Could not fetch employee list");
            return 0;
        }

        JsonArray employees = listDoc["employees"].as<JsonArray>();
        if (employees.isNull()) {
            Serial.println("[Prefetch] ❌ No 'employees' array in response");
            return 0;
        }

        int count = 0;
        for (JsonObject empJson : employees) {
            // Build EmployeeProfile from JSON
            EmployeeProfile emp;
            emp.uid            = _jsonStr(empJson, "uid", "id");
            emp.idNumber       = _jsonStr(empJson, "idNumber", "id_number");
            emp.fullName       = _jsonStr(empJson, "fullName", "full_name");
            emp.firstName      = _jsonStr(empJson, "firstName", "first_name");
            emp.lastName       = _jsonStr(empJson, "lastName", "last_name");
            emp.position       = _jsonStr(empJson, "position", "position");
            emp.department     = _jsonStr(empJson, "department", "department");
            emp.email          = _jsonStr(empJson, "email", "email");
            emp.status         = _jsonStr(empJson, "status", "status");
            emp.employmentType = _jsonStr(empJson, "employmentType", "employment_type");
            emp.accessGranted  = empJson["status"].as<String>() == "Active";
            emp.hasData        = emp.uid.length() > 0;

            if (!emp.hasData || emp.uid.length() == 0) continue;

            // Save/update profile JSON (always refresh from server)
            SDDatabase::saveEmployeeProfile(emp.uid, emp);

            // Download photo only if not already on SD
            if (!SDDatabase::hasPhoto(emp.uid)) {
                Serial.println("[Prefetch] Downloading photo for: " + emp.fullName);
                uint8_t* photoBuf = nullptr;
                int      photoLen = 0;
                if (attService.downloadProfileImage(emp.uid, &photoBuf, &photoLen)) {
                    SDDatabase::savePhoto(emp.uid, photoBuf, (size_t)photoLen);
                    free(photoBuf);
                    Serial.println("[Prefetch] ✅ Photo saved: " + emp.uid);
                } else {
                    Serial.println("[Prefetch] ⚠️ No photo for: " + emp.uid);
                }
            } else {
                Serial.println("[Prefetch] ℹ️ Photo exists: " + emp.uid);
            }

            count++;
            yield();  // prevent WDT
        }

        // Save an index file for quick lookup
        _saveEmployeeIndex(employees);

        Serial.printf("[Prefetch] Done: %d employees cached\n", count);
        return count;
    }

    // ── refreshEmployeePhoto ──────────────────────────────────────────────────
    // Force re-download one employee's photo (called after profile update).
    static bool refreshEmployeePhoto(AttendanceHTTPService& attService,
                                     const String& uid) {
        uint8_t* buf = nullptr;
        int      len = 0;
        if (!attService.downloadProfileImage(uid, &buf, &len)) return false;
        bool ok = SDDatabase::savePhoto(uid, buf, (size_t)len);
        free(buf);
        return ok;
    }

private:
    // ── Load queue from SD into a JSON document ────────────────────────────────
    static void _loadQueue(DynamicJsonDocument& doc) {
        doc.to<JsonArray>();  // ensure array type

        if (!SD_MMC.exists(PENDING_FILE)) return;

        File f = SD_MMC.open(PENDING_FILE, FILE_READ);
        if (!f) return;

        if (f.size() > 0) {
            DeserializationError err = deserializeJson(doc, f);
            if (err) {
                Serial.println("[Sync] Queue parse error: " + String(err.c_str()));
                doc.to<JsonArray>();
            }
        }
        f.close();

        // Ensure it's an array
        if (!doc.is<JsonArray>()) {
            doc.to<JsonArray>();
        }
    }

    // ── Save queue to SD ────────────────────────────────────────────────────────
    static bool _saveQueue(DynamicJsonDocument& doc) {
        File f = SD_MMC.open(PENDING_FILE, FILE_WRITE);
        if (!f) {
            Serial.println("[Sync] Cannot write queue file");
            return false;
        }
        serializeJson(doc, f);
        f.close();
        return true;
    }

    // ── Save employee UID index ────────────────────────────────────────────────
    static void _saveEmployeeIndex(JsonArray& employees) {
        File f = SD_MMC.open(EMPLOYEES_FILE, FILE_WRITE);
        if (!f) return;
        // Save compact array of {uid, nfcAccess, idNumber, fullName}
        DynamicJsonDocument idx(8192);
        JsonArray arr = idx.to<JsonArray>();
        for (JsonObject e : employees) {
            JsonObject item = arr.createNestedObject();
            item["uid"] = _jsonStr(e, "uid", "id");
            item["nfc"] = _jsonStr(e, "nfcAccess", "nfc_access");
            item["id"]  = _jsonStr(e, "idNumber",  "id_number");
            item["n"]   = _jsonStr(e, "fullName",  "full_name");
        }
        serializeJson(idx, f);
        f.close();
        Serial.println("[Prefetch] Employee index saved: " + String(arr.size()) + " entries");
    }

    // ── Helper: read a field trying two key names ──────────────────────────────
    static String _jsonStr(JsonObject obj, const char* key1, const char* key2) {
        if (obj.containsKey(key1) && !obj[key1].isNull()) {
            JsonVariantConst v = obj[key1];
            if (v.is<int>())  return String(v.as<int>());
            if (v.is<long>()) return String(v.as<long>());
            return v | "";
        }
        if (obj.containsKey(key2) && !obj[key2].isNull()) {
            JsonVariantConst v = obj[key2];
            if (v.is<int>())  return String(v.as<int>());
            if (v.is<long>()) return String(v.as<long>());
            return v | "";
        }
        return "";
    }
};