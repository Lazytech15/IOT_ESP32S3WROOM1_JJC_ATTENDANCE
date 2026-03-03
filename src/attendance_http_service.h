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
    // downloadProfileImage
    // KEY FIX: Uses a LOCAL HTTPClient instead of the shared class member `http`.
    // The shared `http` can be mid-session from fetchAllEmployeesEach, which
    // causes downloadProfileImage to silently get 0 bytes. A fresh local
    // HTTPClient always starts clean and connects correctly.
    // KEY FIX: Targets /api/profile/<uid>/default directly to skip 301 redirect.
    // ══════════════════════════════════════════════════════════════════════════════
    bool downloadProfileImage(const String& uid, uint8_t** outBuffer, int* outSize,
                               const String& overridePath = "") {
        if (uid.length() == 0 || !outBuffer || !outSize) return false;

        // Build URL — go straight to /default to skip any server-side redirect
        String url;
        if (overridePath.length() > 0) {
            if (overridePath.startsWith("http://") || overridePath.startsWith("https://"))
                url = overridePath;
            else if (overridePath.startsWith("/"))
                url = serverURL + overridePath;
            else
                url = serverURL + "/" + overridePath;
        } else {
            url = serverURL + "/api/profile/" + uid + "/default";
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

        // If overridePath failed, automatically retry with the /default path
        if (code != 200 && overridePath.length() > 0) {
            Serial.println("[IMG] Retrying with /default path...");
            Serial.flush();
            imgHttp.end();
            String fallback = serverURL + "/api/profile/" + uid + "/default";
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
    bool recordAttendance(const String& employeeUid,
                          const String& nfcUid,
                          const String& deviceId,
                          const String& type = "check-in") {
        Serial.println("\n[HTTP] 📝 Recording attendance...");
        Serial.flush();

        DynamicJsonDocument doc(256);
        doc["employee_uid"] = employeeUid;
        doc["nfc_uid"]      = nfcUid;
        doc["nfc_access"]   = nfcUid;
        doc["device_id"]    = deviceId;
        doc["type"]         = type;
        doc["timestamp"]    = millis();

        String payload;
        serializeJson(doc, payload);

        DynamicJsonDocument respDoc(1024);
        bool ok = postAndDecrypt(serverURL + "/api/attendance", payload, respDoc);
        if (ok && respDoc.containsKey("success"))
            ok = respDoc["success"].as<bool>();

        Serial.println("[HTTP] Attendance " + String(ok ? "recorded OK ✅" : "FAILED ❌"));
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
};