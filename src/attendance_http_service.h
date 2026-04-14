//current project - HTTP SERVICE FOR ATTENDANCE (WITH AES DECRYPTION)
//
// FIX v3:
// - fetchAllEmployeesEach now uses decryptServerResponse() — the same
//   battle-tested path as authenticateNFC() — instead of reimplementing
//   the base64/envelope extraction manually.
// - Page-level DynamicJsonDocument bumped to 65536 bytes so face_descriptor
//   and document fields never cause silent NoMemory failures.
// - Uses _findEmployeesArray() to handle both data.employees and top-level
//   employees envelope shapes.
// - ID field pre-check uses "id" (matching PHP formatEmployeeData) and logs
//   every skipped record so nothing disappears silently.

#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "employee_profile_display.h"
#include "aes_decryptor.h"
#include "sd_database.h"   // needed for SDDatabase::savePhoto() in downloadAllPhotosZip

#define AES_KEY_B64 "wSDp34MhW1pp7RJ8V01ovioEMYKI2hJceZ91VzZcA7s="

class AttendanceHTTPService {
private:
    String       serverURL;
    String       authToken;
    HTTPClient   http;
    AesDecryptor decryptor;

    void addCommonHeaders() {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Client-Type", "ESP32");
        if (authToken.length() > 0)
            http.addHeader("Authorization", "Bearer " + authToken);
    }

    void debugPrintDecrypted(const String& rawEncryptedBody,
                             const DynamicJsonDocument& parsedDoc) {
        Serial.println();
        Serial.println("╔══════════════════════════════════════════════════════════╗");
        Serial.println("║            DECRYPTED SERVER RESPONSE                    ║");
        Serial.println("╠══════════════════════════════════════════════════════════╣");
        String raw = rawEncryptedBody.substring(0, min(120, (int)rawEncryptedBody.length()));
        if (rawEncryptedBody.length() > 120) raw += "...";
        Serial.println("║  " + raw);
        Serial.println("╠══════════════════════════════════════════════════════════╣");
        Serial.println("║ [DECRYPTED JSON]                                         ║");
        String pretty;
        serializeJsonPretty(parsedDoc, pretty);
        int start = 0;
        while (start < (int)pretty.length()) {
            int nl = pretty.indexOf('\n', start);
            String line = (nl == -1) ? pretty.substring(start) : pretty.substring(start, nl);
            Serial.println("║  " + line);
            if (nl == -1) break;
            start = nl + 1;
        }
        Serial.println("╚══════════════════════════════════════════════════════════╝");
        Serial.println();
        Serial.flush();
    }

    bool postAndDecrypt(const String& url,
                        const String& payload,
                        DynamicJsonDocument& outDoc) {
        Serial.println("[HTTP] POST " + url);
        Serial.println("[HTTP] Payload: " + payload);
        Serial.flush();

        http.setTimeout(8000);
        http.begin(url);
        addCommonHeaders();
        int code = http.POST(payload);
        Serial.println("[HTTP] Response code: " + String(code));
        Serial.flush();

        if (code <= 0) {
            Serial.println("[HTTP] ❌ Connection error: " + http.errorToString(code));
            Serial.flush();
            http.end();
            return false;
        }

        String body = http.getString();
        WiFiClient* stream = http.getStreamPtr();
        if (stream) { while (stream->available()) { stream->read(); yield(); } }
        http.end();

        Serial.println("[HTTP] Body length: " + String(body.length()));
        Serial.println("[HTTP] Body (first 200): " + body.substring(0, min(200, (int)body.length())));
        Serial.flush();

        if (code != 200 && code != 403) {
            Serial.println("[HTTP] ❌ Unexpected HTTP code: " + String(code));
            Serial.flush();
            return false;
        }
        if (body.length() == 0) {
            Serial.println("[HTTP] ❌ Empty response body");
            Serial.flush();
            return false;
        }

        yield();
        bool ok = decryptServerResponse(decryptor, body, outDoc);
        if (!ok) {
            Serial.println("[HTTP] ❌ Failed to decrypt/parse response");
            Serial.flush();
        } else {
            debugPrintDecrypted(body, outDoc);
        }
        return ok;
    }

    // ── Helper: given a decrypted doc, find the employees JsonArray ────────────
    // PHP sendSuccessResponse wraps data like:
    //   {"success":true,"data":{"employees":[...],"pagination":{...}}}
    // But some endpoints return the array at the top level:
    //   {"success":true,"employees":[...]}
    // This helper handles both.
    static JsonArray _findEmployeesArray(DynamicJsonDocument& doc) {
        // Try top-level "employees" first
        if (doc.containsKey("employees")) {
            JsonArray arr = doc["employees"].as<JsonArray>();
            if (!arr.isNull()) return arr;
        }
        // Try wrapped "data.employees"
        if (doc.containsKey("data") && doc["data"].is<JsonObject>()) {
            JsonObject data = doc["data"].as<JsonObject>();
            if (data.containsKey("employees")) {
                JsonArray arr = data["employees"].as<JsonArray>();
                if (!arr.isNull()) return arr;
            }
        }
        return JsonArray(); // null array
    }

public:
    AttendanceHTTPService(const String& url   = "https://jjcenggworks.com",
                          const String& token = "")
        : serverURL(url), authToken(token), decryptor(AES_KEY_B64) {
        if (decryptor.isReady()) {
            Serial.println("[HTTP] ✅ AES decryptor ready");
        } else {
            Serial.println("[HTTP] ❌ WARNING: AES decryptor init failed!");
        }
        Serial.flush();
    }

    void setAuthToken(const String& token) { authToken = token; }

    // ══════════════════════════════════════════════════════════════════════════
    // authenticateNFC
    // ══════════════════════════════════════════════════════════════════════════
    bool authenticateNFC(const String& nfcUid,
                         const String& deviceId,
                         EmployeeProfile& employee) {
        Serial.println("\n[HTTP] 🔐 Authenticating NFC: " + nfcUid);
        Serial.flush();

        DynamicJsonDocument reqDoc(256);
        reqDoc["nfc_access"] = nfcUid;
        reqDoc["nfc_uid"]    = nfcUid;
        reqDoc["device_id"]  = deviceId;
        reqDoc["timestamp"]  = millis();

        String payload;
        serializeJson(reqDoc, payload);

        DynamicJsonDocument respDoc(8192);
        if (!postAndDecrypt(serverURL + "/api/nfc-auth", payload, respDoc)) {
            Serial.println("[HTTP] ❌ authenticateNFC failed");
            Serial.flush();
            return false;
        }

        if (respDoc.containsKey("success") && !respDoc["success"].as<bool>()) {
            String msg = respDoc["message"] | respDoc["error"] | "Unknown error";
            Serial.println("[HTTP] ❌ Server error: " + msg);
            Serial.flush();
            return false;
        }

        // Find employee object — may be at top level or under "data"
        JsonObject e;
        if (respDoc.containsKey("employee")) {
            e = respDoc["employee"].as<JsonObject>();
        } else if (respDoc.containsKey("data") && respDoc["data"].is<JsonObject>()) {
            JsonObject d = respDoc["data"].as<JsonObject>();
            if (d.containsKey("employee")) e = d["employee"].as<JsonObject>();
        }

        if (e.isNull()) {
            Serial.println("[HTTP] ❌ No 'employee' key in decrypted response");
            Serial.print("[HTTP] Keys found: ");
            for (JsonPair kv : respDoc.as<JsonObject>())
                Serial.print(String(kv.key().c_str()) + " ");
            Serial.println();
            Serial.flush();
            return false;
        }

        JsonVariantConst uidVar = e["uid"];
        if (uidVar.is<int>())       employee.uid = String(uidVar.as<int>());
        else if (uidVar.is<long>()) employee.uid = String(uidVar.as<long>());
        else                        employee.uid = uidVar | "";

        employee.idNumber       = e["id_number"]       | e["idNumber"]       | "";
        employee.fullName       = e["full_name"]        | e["fullName"]       | "";
        employee.firstName      = e["first_name"]       | e["firstName"]      | "";
        employee.lastName       = e["last_name"]        | e["lastName"]       | "";
        employee.position       = e["position"]         | "";
        employee.department     = e["department"]       | "";
        employee.email          = e["email"]            | "";
        employee.status         = e["status"]           | "";
        employee.employmentType  = e["employment_type"]  | e["employmentType"] | "";
        employee.profilePicture  = e["profile_picture"]  | e["profilePicture"] | "";
        employee.accessGranted   = respDoc["access_granted"] | false;
        employee.authTime        = respDoc["auth_time"] | "";
        employee.hasData         = true;

        Serial.println("[HTTP] ✅ Employee: " + employee.fullName);
        Serial.println("[HTTP] Access: " + String(employee.accessGranted ? "GRANTED ✅" : "DENIED ❌"));
        Serial.flush();
        return employee.accessGranted;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // downloadProfileImage
    // ══════════════════════════════════════════════════════════════════════════════
    // KEY FIX v4: Corrected endpoint from "/api/profile/<uid>/default" (which
    // does NOT exist in the PHP router) to "/api/profile/<uid>" (GET handler
    // that streams the most recent profile image directly — see profile.php).
    //
    // KEY FIX: Uses a LOCAL HTTPClient instead of the shared class member `http`.
    // The shared `http` can be mid-session from fetchAllEmployeesEach, which
    // causes downloadProfileImage to silently get 0 bytes. A fresh local
    // HTTPClient always starts clean and connects correctly.
    // ══════════════════════════════════════════════════════════════════════════════
    bool downloadProfileImage(const String& uid, uint8_t** outBuffer, int* outSize,
                               const String& overridePath = "") {
        if (uid.length() == 0 || !outBuffer || !outSize) return false;

        // Build URL — GET /api/profile/<uid> streams the most recent photo directly.
        // This is the correct route (see profile.php GET /api/profile/:uid handler).
        // "/default" does NOT exist in the PHP router and always returns 404.
        String url;
        if (overridePath.length() > 0) {
            if (overridePath.startsWith("http://") || overridePath.startsWith("https://"))
                url = overridePath;
            else if (overridePath.startsWith("/"))
                url = serverURL + overridePath;
            else
                url = serverURL + "/" + overridePath;
        } else {
            url = serverURL + "/api/profile/" + uid;   // ← correct endpoint
        }

        Serial.println("[IMG] GET " + url);
        Serial.printf("[IMG] heap=%u psram=%u\n", ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.flush();

        // *** CRITICAL: Use a LOCAL HTTPClient — do NOT use the shared `http` ***
        // The class-level `http` may still have an open connection from the
        // paginated fetchAllEmployeesEach loop, making any GET here silently
        // return 0 bytes. A local instance always starts with a clean state.
        HTTPClient imgHttp;
        imgHttp.setTimeout(15000);
        imgHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        imgHttp.begin(url);
        imgHttp.addHeader("X-Client-Type", "ESP32");
        if (authToken.length() > 0)
            imgHttp.addHeader("Authorization", "Bearer " + authToken);

        const char* headerKeys[] = {"Content-Type", "Content-Length"};
        imgHttp.collectHeaders(headerKeys, 2);

        int code = imgHttp.GET();
        Serial.println("[IMG] Code: " + String(code));
        Serial.flush();

        // If overridePath failed, retry with the canonical /api/profile/<uid>
        if (code != 200 && overridePath.length() > 0) {
            Serial.println("[IMG] Retrying with canonical /api/profile/<uid>...");
            Serial.flush();
            imgHttp.end();
            String fallback = serverURL + "/api/profile/" + uid;
            imgHttp.setTimeout(15000);
            imgHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            imgHttp.begin(fallback);
            imgHttp.addHeader("X-Client-Type", "ESP32");
            imgHttp.collectHeaders(headerKeys, 2);
            code = imgHttp.GET();
            Serial.println("[IMG] Retry code: " + String(code));
            Serial.flush();
        }

        if (code != 200) {
            Serial.println("[IMG] FAILED: HTTP " + String(code));
            Serial.flush();
            imgHttp.end();
            return false;
        }

        // Content-Type — only block clearly non-image responses
        String ct = imgHttp.header("Content-Type");
        Serial.println("[IMG] Content-Type: " + ct);
        if (ct.length() > 0 && ct.indexOf("image") < 0 &&
            ct.indexOf("octet-stream") < 0 && ct.indexOf("jpeg") < 0) {
            Serial.println("[IMG] FAILED: not an image Content-Type: " + ct);
            Serial.flush();
            imgHttp.end();
            return false;
        }

        int size = imgHttp.getSize();
        Serial.printf("[IMG] Content-Length: %d\n", size);
        const int MAX_IMG_BYTES = 500000;
        if (size > MAX_IMG_BYTES) {
            Serial.println("[IMG] FAILED: too large " + String(size));
            Serial.flush();
            imgHttp.end();
            return false;
        }

        WiFiClient* stream = imgHttp.getStreamPtr();
        if (!stream) {
            Serial.println("[IMG] FAILED: no stream");
            Serial.flush();
            imgHttp.end();
            return false;
        }

        // Allocate buffer — PSRAM preferred
        int allocSize = (size > 0) ? (size + 16) : 65536;
        uint8_t* buf = nullptr;
        if (psramFound()) buf = (uint8_t*)ps_malloc(allocSize);
        if (!buf)         buf = (uint8_t*)malloc(allocSize);
        if (!buf) {
            Serial.printf("[IMG] FAILED: alloc %d B  heap=%u psram=%u\n",
                          allocSize, ESP.getFreeHeap(), ESP.getFreePsram());
            Serial.flush();
            imgHttp.end();
            return false;
        }

        // Stream read
        int got = 0;
        unsigned long lastData = millis();
        const int CHUNK = 4096;
        while (true) {
            if (millis() - lastData > 12000) {
                Serial.println("[IMG] FAILED: timeout");
                Serial.flush();
                free(buf); imgHttp.end(); return false;
            }
            int avail = stream->available();
            if (avail > 0) {
                if (got + avail > allocSize) {
                    int newSz = min(got + avail + 32768, MAX_IMG_BYTES);
                    uint8_t* nb = (uint8_t*)realloc(buf, newSz);
                    if (nb) { buf = nb; allocSize = newSz; }
                    else avail = allocSize - got;
                    if (avail <= 0) break;
                }
                int rd = stream->readBytes(buf + got, min(avail, min(CHUNK, allocSize - got)));
                if (rd > 0) { got += rd; lastData = millis(); }
            } else {
                if (size > 0 && got >= size) break;
                if (!imgHttp.connected()) break;
                delay(5);
            }
            if (size > 0 && got >= size) break;
            if (got >= MAX_IMG_BYTES) break;
            yield();
        }
        imgHttp.end();

        Serial.printf("[IMG] Stream done: got=%d expected=%d\n", got, size);
        Serial.flush();

        if (got == 0) {
            Serial.println("[IMG] FAILED: 0 bytes received");
            Serial.flush();
            free(buf); return false;
        }

        // Magic-byte validation — JPEG / PNG / GIF / WebP
        bool validImage = false;
        if (got >= 4) {
            if (buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)                                        validImage = true; // JPEG
            else if (buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47)                 validImage = true; // PNG
            else if (buf[0] == 0x47 && buf[1] == 0x49 && buf[2] == 0x46)                                    validImage = true; // GIF
            else if (got >= 12 && buf[0]==0x52 && buf[1]==0x49 && buf[8]==0x57 && buf[9]==0x45)             validImage = true; // WebP
        }
        if (!validImage) {
            Serial.print("[IMG] FAILED: bad magic bytes (hex): ");
            for (int i = 0; i < min(got, 16); i++) Serial.printf("%02X ", buf[i]);
            Serial.println();
            char preview[201]; int plen = min(got, 200);
            memcpy(preview, buf, plen); preview[plen] = 0;
            Serial.println("[IMG] As text: " + String(preview));
            Serial.flush();
            free(buf); return false;
        }

        Serial.printf("[IMG] OK: %d bytes downloaded\n", got);
        Serial.flush();
        *outBuffer = buf;
        *outSize   = got;
        return true;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // recordAttendance
    // ══════════════════════════════════════════════════════════════════════════
    // Records a single attendance tap to the server.
    // clockType must be the exact column name the server expects:
    //   morning_in | morning_out | afternoon_in | afternoon_out |
    //   evening_in | evening_out
    // clockTimeStr: "HH:MM:SS"   dateStr: "YYYY-MM-DD"
    bool recordAttendance(const String& employeeUid,
                          const String& nfcUid,
                          const String& deviceId,
                          const String& clockType,
                          const String& clockTimeStr = "",
                          const String& dateStr      = "") {
        Serial.println("\n[HTTP] 📝 Recording attendance: " + clockType);
        Serial.flush();

        // Build full datetime string: "YYYY-MM-DD HH:MM:SS"
        String clockTimeFull = (dateStr.length() > 0 && clockTimeStr.length() > 0)
                               ? (dateStr + " " + clockTimeStr)
                               : clockTimeStr;

        DynamicJsonDocument doc(512);
        doc["employee_uid"] = employeeUid;
        doc["nfc_uid"]      = nfcUid;
        doc["nfc_access"]   = nfcUid;
        doc["device_id"]    = deviceId;
        // ── Fields the server attendance.php createAttendanceRecord() reads ──
        doc["clock_type"]   = clockType;          // exact column name
        doc["clock_time"]   = clockTimeFull;      // full datetime for DB
        doc["date"]         = dateStr.length() > 0 ? dateStr : clockTimeStr.substring(0,10);
        doc["is_synced"]    = 1;
        doc["timestamp"]    = millis();

        String payload;
        serializeJson(doc, payload);

        Serial.println("[HTTP] Payload: " + payload);
        Serial.flush();

        DynamicJsonDocument respDoc(1024);
        bool ok = postAndDecrypt(serverURL + "/api/attendance/record", payload, respDoc);
        if (ok && respDoc.containsKey("success"))
            ok = respDoc["success"].as<bool>();

        Serial.println("[HTTP] Attendance " + String(ok ? "recorded OK" : "FAILED"));
        if (!ok && respDoc.containsKey("message"))
            Serial.println("[HTTP] Error: " + String(respDoc["message"] | ""));

        // Capture server record ID and persist to SD map.
        // Future edits use PUT /{id} instead of POST -> no duplicate rows.
        if (ok) {
            int serverId = 0;
            if (respDoc["data"]["id"].is<int>())      serverId = respDoc["data"]["id"] | 0;
            else if (respDoc["id"].is<int>())          serverId = respDoc["id"] | 0;

            if (serverId > 0 && dateStr.length() == 10) {
                SDDatabase::saveServerIdMapping(dateStr, employeeUid,
                                                clockType, clockTimeStr, serverId);
                Serial.printf("[HTTP] server_id=%d saved to SD map\n", serverId);
            } else {
                Serial.printf("[HTTP] No server_id in response (id=%d)\n", serverId);
            }
        }

        Serial.flush();
        return ok;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // fetchAllEmployees — simple one-shot fetch (small rosters only)
    // ══════════════════════════════════════════════════════════════════════════
    bool fetchAllEmployees(DynamicJsonDocument& outDoc) {
        String url = serverURL + "/api/employees";
        Serial.println("[HTTP] fetchAllEmployees: " + url);
        Serial.flush();

        http.setTimeout(15000);
        http.begin(url);
        http.addHeader("X-Client-Type", "ESP32");
        if (authToken.length() > 0)
            http.addHeader("Authorization", "Bearer " + authToken);

        int code = http.GET();
        Serial.println("[HTTP] fetchAllEmployees code: " + String(code));
        Serial.flush();

        if (code <= 0) { http.end(); return false; }
        String body = http.getString();
        http.end();

        Serial.printf("[HTTP] fetchAllEmployees body len: %d\n", (int)body.length());
        if (body.length() > 0)
            Serial.println("[HTTP] Preview: " + body.substring(0, min(200,(int)body.length())));
        Serial.flush();

        if (code != 200 && code != 403) return false;
        if (body.length() == 0) return false;
        yield();

        DynamicJsonDocument tempDoc(32768);
        bool ok = decryptServerResponse(decryptor, body, tempDoc);
        if (!ok) {
            Serial.println("[HTTP] ❌ fetchAllEmployees decrypt failed");
            Serial.flush();
            return false;
        }

        JsonArray arr = _findEmployeesArray(tempDoc);
        if (arr.isNull()) {
            Serial.println("[HTTP] ❌ fetchAllEmployees: 'employees' array not found");
            Serial.println("[HTTP] Top-level keys:");
            for (JsonPair kv : tempDoc.as<JsonObject>())
                Serial.println("  " + String(kv.key().c_str()));
            Serial.flush();
            return false;
        }

        outDoc["employees"] = arr;

        int cnt = arr.size();
        Serial.println("[HTTP] fetchAllEmployees: " + String(cnt) + " employees");
        Serial.flush();
        return true;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // decryptBody  (public helper)
    // ══════════════════════════════════════════════════════════════════════════
    bool decryptBody(const String& rawBody, DynamicJsonDocument& outDoc) {
        return decryptServerResponse(decryptor, rawBody, outDoc);
    }

    String getServerURL() const { return serverURL; }

    bool sendButtonEvent(const String& action,
                         const String& deviceId,
                         const String& nfcUid = "") {
        DynamicJsonDocument doc(256);
        doc["button_action"] = action;
        doc["device_id"]     = deviceId;
        doc["timestamp"]     = millis();
        if (nfcUid.length() > 0) doc["nfc_uid"] = nfcUid;
        String payload;
        serializeJson(doc, payload);
        DynamicJsonDocument respDoc(512);
        return postAndDecrypt(serverURL + "/api/nfc-auth", payload, respDoc);
    }

    // ══════════════════════════════════════════════════════════════════════════
    // fetchAllEmployeesEach — paginated streaming fetch
    //
    // FIX v3:
    //   • Uses decryptServerResponse() — same path as authenticateNFC() which
    //     is known-good — instead of reimplementing envelope extraction.
    //   • Page DynamicJsonDocument = 65536 bytes: handles face_descriptor /
    //     document fields that blew past the old 8192-byte budget causing
    //     silent NoMemory parse failures (= zero employees delivered).
    //   • Uses _findEmployeesArray() to unwrap data.employees OR top-level.
    //   • Pre-checks "id" field (PHP formatEmployeeData returns int "id", not
    //     "uid") and logs every skipped record so nothing disappears silently.
    // ══════════════════════════════════════════════════════════════════════════
    template<typename Callback>
    int fetchAllEmployeesEach(Callback onEmployee) {
        // PAGE = 5: smaller pages keep encrypted payload small for AES decrypt
        // AND keep pageDoc within budget. face_descriptor + document fields
        // can push one employee to 2-4 KB of JSON; 10 × 4 KB = 40 KB which
        // silently blows the old 24576-byte pageDoc causing NoMemory → 0 results.
        // 5 × 4 KB = 20 KB fits easily in the 65536-byte pageDoc below.
        const int PAGE = 5;
        int offset = 0, total = 0;

        Serial.println("[HTTP] ===== fetchAllEmployeesEach START =====");
        Serial.printf("[HTTP] Heap: %u free  PSRAM: %u free\n",
                      ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.flush();

        while (true) {
            String url = serverURL +
                "/api/employees?includeAllStatuses=true&limit=" +
                String(PAGE) + "&offset=" + String(offset);

            Serial.println("[HTTP] GET " + url);
            Serial.flush();

            http.setTimeout(20000);
            http.begin(url);
            http.addHeader("X-Client-Type", "ESP32");
            if (authToken.length() > 0)
                http.addHeader("Authorization", "Bearer " + authToken);

            int code = http.GET();
            Serial.printf("[HTTP] Code: %d\n", code);
            Serial.flush();

            if (code <= 0) {
                Serial.println("[HTTP] Connection error: " + http.errorToString(code));
                Serial.flush();
                http.end();
                break;
            }

            String raw = http.getString();
            {
                WiFiClient* s = http.getStreamPtr();
                if (s) while (s->available()) { s->read(); yield(); }
            }
            http.end();

            Serial.printf("[HTTP] Raw body: %d bytes\n", (int)raw.length());
            if (raw.length() > 0)
                Serial.println("[HTTP] Raw preview: " +
                               raw.substring(0, min(200, (int)raw.length())));
            Serial.flush();

            if (code != 200 && code != 403) {
                Serial.printf("[HTTP] Unexpected HTTP code %d — stopping\n", code);
                Serial.flush();
                break;
            }
            if (raw.length() == 0) {
                Serial.println("[HTTP] Empty response — stopping");
                Serial.flush();
                break;
            }

            // ── Decrypt via the proven decryptServerResponse() path ────────────
            // PAGE=10 keeps ciphertext small. aes_decryptor.h now uses
            // ps_malloc (PSRAM) for decode+plaintext buffers so even tight
            // heap situations succeed. pageDoc=24576 fits 10 full employees.
            Serial.printf("[HTTP] Pre-decrypt heap: %u  psram: %u\n",
                          ESP.getFreeHeap(), ESP.getFreePsram());
            Serial.flush();
            DynamicJsonDocument pageDoc(65536);
            bool ok = decryptServerResponse(decryptor, raw, pageDoc);
            raw = "";   // free raw immediately — no longer needed
            yield();

            if (!ok) {
                Serial.println("[HTTP] decryptServerResponse failed — stopping");
                Serial.flush();
                break;
            }

            Serial.printf("[HTTP] Page doc memory used: %u bytes\n",
                          (unsigned)pageDoc.memoryUsage());
            Serial.flush();

            // ── Unwrap envelope: data.employees OR top-level employees ─────────
            JsonArray arr = _findEmployeesArray(pageDoc);
            if (arr.isNull()) {
                Serial.println("[HTTP] 'employees' array not found — diagnosing:");
                Serial.print("[HTTP] Top-level keys: ");
                for (JsonPair kv : pageDoc.as<JsonObject>())
                    Serial.print(String(kv.key().c_str()) + " ");
                Serial.println();
                if (pageDoc.containsKey("data") && pageDoc["data"].is<JsonObject>()) {
                    Serial.print("[HTTP] data.* keys: ");
                    for (JsonPair kv : pageDoc["data"].as<JsonObject>())
                        Serial.print(String(kv.key().c_str()) + " ");
                    Serial.println();
                }
                Serial.flush();
                break;
            }

            int pageCount = arr.size();
            Serial.printf("[HTTP] Page has %d employees\n", pageCount);
            Serial.flush();

            if (pageCount == 0) break;  // empty page = all done

            // ── Walk array, fire callback per employee ────────────────────────
            int pageIdx = 0;
            for (JsonObject emp : arr) {
                pageIdx++;

                // Print first 3 to verify field names coming from server
                if (total + pageIdx <= 3) {
                    String dbg;
                    serializeJson(emp, dbg);
                    Serial.println("[HTTP] emp#" + String(total + pageIdx) +
                                   " sample: " +
                                   dbg.substring(0, min(300, (int)dbg.length())));
                    Serial.flush();
                }

                // PHP formatEmployeeData returns integer key "id" (not "uid").
                // Verify it exists and is non-zero before firing the callback.
                String empId = "";
                if (emp.containsKey("id") && !emp["id"].isNull()) {
                    JsonVariantConst v = emp["id"];
                    if (v.is<int>())       empId = String(v.as<int>());
                    else if (v.is<long>()) empId = String(v.as<long>());
                    else                   empId = v.as<String>();
                }

                if (empId.length() == 0 || empId == "0") {
                    String fn = emp["fullName"] | emp["full_name"] | "(none)";
                    Serial.printf("[HTTP] SKIP emp#%d — id missing/zero fullName=%s\n",
                                  total + pageIdx, fn.c_str());
                    Serial.flush();
                    yield();
                    continue;
                }

                onEmployee(emp);
                total++;
                yield();
            }

            Serial.printf("[HTTP] Page offset=%d processed=%d running_total=%d heap=%u\n",
                          offset, pageIdx, total, ESP.getFreeHeap());
            Serial.flush();

            offset += pageCount;
            if (pageCount < PAGE) break;   // last (partial) page
            if (offset >= 5000)  break;    // safety cap

            delay(100);
        }

        Serial.printf("[HTTP] ===== fetchAllEmployeesEach DONE: %d =====\n", total);
        Serial.flush();
        return total;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // downloadAllPhotosZip
    // ══════════════════════════════════════════════════════════════════════════
    // Downloads ALL employee profile photos in a single HTTP request as a ZIP
    // archive from POST /api/profile/bulk/download, then extracts each JPEG
    // directly to /photos/<uid>.jpg on the SD card.
    //
    // This replaces the 1-per-employee HTTP loop used previously and cuts total
    // download time from minutes to seconds for a typical 50-employee roster.
    //
    // ZIP format assumed: flat archive where each entry filename is
    //   <uid>_<FirstName>_<LastName>.jpg   (as the PHP bulk/download produces)
    //
    // Returns number of photos successfully saved to SD.
    // ══════════════════════════════════════════════════════════════════════════
    int downloadAllPhotosZip(const String* uidArray, int uidCount,
                              void (*progressCb)(int cur, int total, const String& uid) = nullptr) {
        if (uidCount == 0) return 0;

        // ── Build POST body: { "uids": [1, 2, ...] } ──────────────────────────
        // Use a char buffer to avoid ArduinoJson overhead for a plain int array.
        String body = "{\"uids\":[";
        for (int i = 0; i < uidCount; i++) {
            body += uidArray[i];
            if (i < uidCount - 1) body += ",";
        }
        body += "],\"include_summary\":false}";

        String url = serverURL + "/api/profile/bulk/download";
        Serial.println("[ZIP] POST " + url);
        Serial.printf("[ZIP] Requesting %d photos  heap=%u psram=%u\n",
                      uidCount, ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.flush();

        HTTPClient zipHttp;
        zipHttp.setTimeout(60000);   // bulk download can take a while
        zipHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        zipHttp.begin(url);
        zipHttp.addHeader("Content-Type", "application/json");
        zipHttp.addHeader("X-Client-Type", "ESP32");
        if (authToken.length() > 0)
            zipHttp.addHeader("Authorization", "Bearer " + authToken);

        int code = zipHttp.POST(body);
        Serial.printf("[ZIP] HTTP code: %d\n", code);
        Serial.flush();

        if (code != 200 && code != 201) {
            Serial.println("[ZIP] FAILED: unexpected HTTP code " + String(code));
            String errBody = zipHttp.getString();
            Serial.println("[ZIP] Error body: " + errBody.substring(0, 200));
            Serial.flush();
            zipHttp.end();
            return 0;
        }

        int zipSize = zipHttp.getSize();
        Serial.printf("[ZIP] Content-Length: %d\n", zipSize);
        Serial.flush();

        // ── Allocate buffer for the entire ZIP (PSRAM preferred) ──────────────
        const int MAX_ZIP = 10 * 1024 * 1024;  // 10 MB cap
        if (zipSize > MAX_ZIP) {
            Serial.printf("[ZIP] FAILED: ZIP too large (%d B)\n", zipSize);
            zipHttp.end();
            return 0;
        }

        int allocSize = (zipSize > 0) ? (zipSize + 256) : 524288;  // default 512 KB
        uint8_t* zipBuf = nullptr;
        if (psramFound()) zipBuf = (uint8_t*)ps_malloc(allocSize);
        if (!zipBuf)      zipBuf = (uint8_t*)malloc(allocSize);
        if (!zipBuf) {
            Serial.printf("[ZIP] FAILED: alloc %d B  heap=%u psram=%u\n",
                          allocSize, ESP.getFreeHeap(), ESP.getFreePsram());
            Serial.flush();
            zipHttp.end();
            return 0;
        }

        // ── Stream entire ZIP into buffer ─────────────────────────────────────
        WiFiClient* stream = zipHttp.getStreamPtr();
        int got = 0;
        unsigned long lastData = millis();
        const int CHUNK = 4096;
        while (true) {
            if (millis() - lastData > 30000) {
                Serial.println("[ZIP] TIMEOUT reading stream");
                Serial.flush();
                break;
            }
            int avail = stream ? stream->available() : 0;
            if (avail > 0) {
                if (got + avail > allocSize) {
                    int newSz = min(got + avail + 65536, MAX_ZIP);
                    uint8_t* nb = psramFound() ? (uint8_t*)ps_realloc(zipBuf, newSz)
                                               : (uint8_t*)realloc(zipBuf, newSz);
                    if (nb) { zipBuf = nb; allocSize = newSz; }
                    else    { avail = allocSize - got; if (avail <= 0) break; }
                }
                int rd = stream->readBytes(zipBuf + got,
                                            min(avail, min(CHUNK, allocSize - got)));
                if (rd > 0) { got += rd; lastData = millis(); }
            } else {
                if (zipSize > 0 && got >= zipSize) break;
                if (!zipHttp.connected()) break;
                delay(10);
            }
            if (zipSize > 0 && got >= zipSize) break;
            if (got >= MAX_ZIP) break;
            yield();
        }
        zipHttp.end();

        Serial.printf("[ZIP] Stream done: got=%d expected=%d\n", got, zipSize);
        Serial.flush();

        if (got < 22) {  // ZIP local file header minimum
            Serial.println("[ZIP] FAILED: data too short to be a ZIP");
            free(zipBuf);
            return 0;
        }

        // ── Validate PK magic bytes ────────────────────────────────────────────
        if (zipBuf[0] != 0x50 || zipBuf[1] != 0x4B) {
            Serial.printf("[ZIP] FAILED: not a ZIP (magic %02X %02X). Preview: ", zipBuf[0], zipBuf[1]);
            char preview[201]; int plen = min(got, 200);
            memcpy(preview, zipBuf, plen); preview[plen] = 0;
            Serial.println(String(preview));
            Serial.flush();
            free(zipBuf);
            return 0;
        }

        // ── Parse ZIP local file headers and extract each image ───────────────
        // ZIP local file header layout (APPNOTE §4.3.7):
        //   Offset  Len  Field
        //      0     4   Signature  0x04034B50
        //      4     2   Version needed
        //      6     2   General purpose bit flag
        //      8     2   Compression method  (0=stored, 8=deflate)
        //     10     2   Last mod time
        //     12     2   Last mod date
        //     14     4   CRC-32
        //     18     4   Compressed size
        //     22     4   Uncompressed size
        //     26     2   File name length
        //     28     2   Extra field length
        //     30     n   File name
        //    30+n    m   Extra field
        //  30+n+m   cs   File data
        //
        // We only support method 0 (stored) because ESP32 has no zlib decompressor
        // in Arduino-land without an extra library.  The PHP bulk/download handler
        // uses ZipArchive::CREATE which defaults to DEFLATE.  We therefore request
        // the server send stored files by adding "compression_level":0 in the body.

        // Re-issue the request with compression_level:0 if the first entry is compressed
        // (i.e. method byte at offset 8 is non-zero).
        // Actually let's re-request with level 0 upfront for reliability:
        // (We already have the data — just check if stored; if compressed, re-fetch.)

        bool needRedownload = false;
        if (got >= 10) {
            uint16_t method = (uint16_t)zipBuf[8] | ((uint16_t)zipBuf[9] << 8);
            if (method != 0) {
                Serial.printf("[ZIP] Compressed (method=%d) — re-fetching with compression_level:0\n", method);
                Serial.flush();
                needRedownload = true;
            }
        }

        if (needRedownload) {
            // Re-request with explicit compression_level:0 so files are STORED
            body = "{\"uids\":[";
            for (int i = 0; i < uidCount; i++) {
                body += uidArray[i];
                if (i < uidCount - 1) body += ",";
            }
            body += "],\"include_summary\":false,\"compression_level\":0}";

            HTTPClient zipHttp2;
            zipHttp2.setTimeout(60000);
            zipHttp2.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            zipHttp2.begin(url);
            zipHttp2.addHeader("Content-Type", "application/json");
            zipHttp2.addHeader("X-Client-Type", "ESP32");
            if (authToken.length() > 0)
                zipHttp2.addHeader("Authorization", "Bearer " + authToken);

            int code2 = zipHttp2.POST(body);
            Serial.printf("[ZIP] Re-fetch code: %d\n", code2);
            Serial.flush();

            if (code2 == 200 || code2 == 201) {
                int zipSize2 = zipHttp2.getSize();
                if (zipSize2 > 0 && zipSize2 <= MAX_ZIP) {
                    uint8_t* nb = psramFound() ? (uint8_t*)ps_realloc(zipBuf, zipSize2 + 256)
                                               : (uint8_t*)realloc(zipBuf, zipSize2 + 256);
                    if (nb) { zipBuf = nb; allocSize = zipSize2 + 256; }
                }
                WiFiClient* s2 = zipHttp2.getStreamPtr();
                got = 0; lastData = millis();
                while (true) {
                    if (millis() - lastData > 30000) break;
                    int av = s2 ? s2->available() : 0;
                    if (av > 0) {
                        if (got + av > allocSize) break;
                        int rd = s2->readBytes(zipBuf + got, min(av, min(CHUNK, allocSize - got)));
                        if (rd > 0) { got += rd; lastData = millis(); }
                    } else {
                        if (zipHttp2.connected() == false) break;
                        delay(10);
                    }
                    if (zipSize2 > 0 && got >= zipSize2) break;
                    yield();
                }
                zipHttp2.end();
                Serial.printf("[ZIP] Re-fetch done: got=%d\n", got);
                Serial.flush();
            } else {
                zipHttp2.end();
            }
        }

        // ── Walk local file headers ────────────────────────────────────────────
        int saved = 0;
        int pos   = 0;
        int entry = 0;

        while (pos + 30 <= got) {
            // Check local file header signature
            uint32_t sig = (uint32_t)zipBuf[pos]        |
                           ((uint32_t)zipBuf[pos + 1] << 8)  |
                           ((uint32_t)zipBuf[pos + 2] << 16) |
                           ((uint32_t)zipBuf[pos + 3] << 24);

            if (sig == 0x02014B50 || sig == 0x06054B50) break;  // central dir / EOCD
            if (sig != 0x04034B50) { pos++; continue; }          // skip non-header bytes

            uint16_t method      = (uint16_t)zipBuf[pos +  8] | ((uint16_t)zipBuf[pos +  9] << 8);
            uint32_t compSize    = (uint32_t)zipBuf[pos + 18] | ((uint32_t)zipBuf[pos + 19] << 8)
                                 | ((uint32_t)zipBuf[pos + 20] << 16) | ((uint32_t)zipBuf[pos + 21] << 24);
            uint32_t uncompSize  = (uint32_t)zipBuf[pos + 22] | ((uint32_t)zipBuf[pos + 23] << 8)
                                 | ((uint32_t)zipBuf[pos + 24] << 16) | ((uint32_t)zipBuf[pos + 25] << 24);
            uint16_t fnLen       = (uint16_t)zipBuf[pos + 26] | ((uint16_t)zipBuf[pos + 27] << 8);
            uint16_t extraLen    = (uint16_t)zipBuf[pos + 28] | ((uint16_t)zipBuf[pos + 29] << 8);

            int dataOffset = pos + 30 + fnLen + extraLen;

            // Extract filename
            char fname[256] = {0};
            int  fnCopy = min((int)fnLen, 255);
            memcpy(fname, zipBuf + pos + 30, fnCopy);
            fname[fnCopy] = 0;
            String filename(fname);

            entry++;
            Serial.printf("[ZIP] Entry %d: '%s' method=%d stored=%u\n",
                          entry, fname, method, uncompSize);
            Serial.flush();

            if (dataOffset + (int)compSize > got) {
                Serial.println("[ZIP] Entry extends beyond buffer — stopping");
                Serial.flush();
                break;
            }

            // Skip non-image files (e.g. download_summary.json)
            String fnLower = filename; fnLower.toLowerCase();
            bool isImg = fnLower.endsWith(".jpg") || fnLower.endsWith(".jpeg") ||
                         fnLower.endsWith(".png") || fnLower.endsWith(".webp") ||
                         fnLower.endsWith(".gif");

            if (!isImg) {
                Serial.println("[ZIP] Skip non-image: " + filename);
                pos = dataOffset + compSize;
                continue;
            }

            // Extract UID: PHP names files "<uid>_FirstName_LastName.ext"
            // So UID is everything before the first '_'.
            String uid = "";
            int underscore = filename.indexOf('_');
            if (underscore > 0) {
                uid = filename.substring(0, underscore);
            }

            if (uid.length() == 0) {
                Serial.println("[ZIP] Cannot extract UID from filename: " + filename);
                pos = dataOffset + compSize;
                continue;
            }

            const uint8_t* imgData = zipBuf + dataOffset;
            uint32_t       imgLen  = (method == 0) ? uncompSize : compSize;

            if (progressCb) progressCb(entry, uidCount, uid);

            // Validate image magic bytes
            bool validImg = false;
            if (imgLen >= 4) {
                if (imgData[0] == 0xFF && imgData[1] == 0xD8 && imgData[2] == 0xFF)             validImg = true;
                else if (imgData[0] == 0x89 && imgData[1] == 0x50 && imgData[2] == 0x4E)       validImg = true;
                else if (imgData[0] == 0x47 && imgData[1] == 0x49 && imgData[2] == 0x46)       validImg = true;
                else if (imgLen >= 12 && imgData[0] == 0x52 && imgData[1] == 0x49 &&
                         imgData[8] == 0x57 && imgData[9] == 0x45)                              validImg = true;
            }

            if (!validImg) {
                Serial.printf("[ZIP] Bad magic for uid=%s: %02X %02X %02X %02X\n",
                              uid.c_str(), imgData[0], imgData[1], imgData[2], imgData[3]);
                pos = dataOffset + compSize;
                continue;
            }

            // Save to SD
            if (SDDatabase::savePhoto(uid, imgData, (size_t)imgLen)) {
                saved++;
                Serial.printf("[ZIP] ✅ Saved uid=%s (%u bytes)\n", uid.c_str(), imgLen);
            } else {
                Serial.printf("[ZIP] ❌ savePhoto failed uid=%s\n", uid.c_str());
            }
            Serial.flush();

            pos = dataOffset + compSize;
            yield();
        }

        free(zipBuf);

        Serial.printf("[ZIP] Extract done: %d/%d photos saved\n", saved, entry);
        Serial.flush();
        return saved;
    }
};