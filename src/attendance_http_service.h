#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "employee_profile_display.h"
#include "aes_decryptor.h"
#include "sd_database.h"

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

    // ── readHttpBodyReliable ──────────────────────────────────────────────────
    // Replaces getString() which silently truncates on ESP32 without PSRAM.
    //
    // getString() internally stops when its TCP buffer runs out of contiguous
    // heap, returning a partial body with no error — this causes "IncompleteInput"
    // JSON parse failures on responses > ~16KB without PSRAM.
    //
    // This helper reads the stream in CHUNK_SZ-byte blocks, appending each to a
    // pre-reserved String.  It continues until Content-Length is satisfied, the
    // server closes the connection, or the per-chunk watchdog fires.
    //
    // maxBytes: hard cap (caller's practical limit, e.g. 49152 for 48KB).
    // Returns "" on timeout/error with 0 bytes received.
    // ──────────────────────────────────────────────────────────────────────────
    // ── readHttpBodyReliable ──────────────────────────────────────────────────
    // Reads the HTTP response body correctly for BOTH Content-Length and
    // Transfer-Encoding: chunked responses (Content-Length = -1).
    //
    // ROOT CAUSE of "4000 {..." corruption:
    //   When the server sends chunked encoding, each chunk is preceded by its
    //   hex size + CRLF, e.g.:  "4000\r\n{...16384 bytes of JSON...}\r\n"
    //   A raw stream reader sees "4000\r\n" as part of the body, so the body
    //   starts with "4000" not "{", making isEncrypted=NO and JSON parse fail
    //   with EmptyInput (plainJson is "" after the non-JSON prefix is rejected).
    //
    // FIX: When Content-Length == -1 (chunked), use dechunked reading which
    //   parses the hex chunk-size header, reads exactly that many bytes of
    //   payload, skips the trailing CRLF, and repeats until a zero-size chunk.
    //   When Content-Length > 0, use the simple length-based reader.
    //
    // maxBytes: hard cap on total payload accepted.
    // Returns the raw JSON body string (no chunk headers), or "" on error.
    // ──────────────────────────────────────────────────────────────────────────
    static String readHttpBodyReliable(HTTPClient& client, int maxBytes = 65536) {
        int contentLen = client.getSize();   // -1 means chunked / unknown
        Serial.printf("[ReadBody] Content-Length=%d  maxBytes=%d  heap=%u\n",
                      contentLen, maxBytes, ESP.getFreeHeap());
        Serial.flush();

        WiFiClient* stream = client.getStreamPtr();
        if (!stream) {
            Serial.println("[ReadBody] ERROR: no stream pointer");
            Serial.flush();
            return "";
        }

        if (contentLen < 0) {
            // ── Chunked transfer encoding path ──────────────────────────────
            return _readChunked(stream, maxBytes);
        } else {
            // ── Content-Length known path ────────────────────────────────────
            return _readFixed(stream, client, contentLen, maxBytes);
        }
    }

    // ── _readChunked ──────────────────────────────────────────────────────────
    // Parses HTTP/1.1 chunked transfer encoding from the raw TCP stream.
    // Each chunk:  <hex-size>\r\n<payload>\r\n
    // Terminator:  0\r\n\r\n
    // ──────────────────────────────────────────────────────────────────────────
    static String _readChunked(WiFiClient* stream, int maxBytes) {
        const uint32_t TIMEOUT_MS  = 12000;  // total patience
        const uint32_t IDLE_MS     = 5;

        String body;
        body.reserve(min(maxBytes, 32768));

        uint32_t deadline = millis() + TIMEOUT_MS;
        int      totalReceived = 0;

        while (millis() < deadline) {
            // ── Read chunk-size line ─────────────────────────────────────────
            String sizeLine = "";
            bool   gotCR    = false;
            while (millis() < deadline) {
                if (!stream->available()) { delay(IDLE_MS); yield(); continue; }
                char c = (char)stream->read();
                if (c == '\r') { gotCR = true; continue; }
                if (c == '\n' && gotCR) break;   // end of size line
                gotCR = false;
                sizeLine += c;
            }
            sizeLine.trim();
            if (sizeLine.length() == 0) { delay(IDLE_MS); yield(); continue; }

            // Strip chunk extensions (e.g. "1A2B;ext=val" → "1A2B")
            int semiColon = sizeLine.indexOf(';');
            if (semiColon >= 0) sizeLine = sizeLine.substring(0, semiColon);

            long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
            Serial.printf("[ReadBody] Chunk size: %ld (0x%s)\n",
                          chunkSize, sizeLine.c_str());
            Serial.flush();

            if (chunkSize == 0) {
                // Terminal chunk — consume trailing CRLF and done
                break;
            }
            if (chunkSize < 0 || chunkSize > maxBytes) {
                Serial.printf("[ReadBody] Chunk too large or invalid: %ld\n", chunkSize);
                Serial.flush();
                break;
            }

            // ── Read exactly chunkSize bytes of payload ──────────────────────
            int remaining = (int)chunkSize;
            while (remaining > 0 && millis() < deadline) {
                int avail = stream->available();
                if (avail <= 0) { delay(IDLE_MS); yield(); continue; }

                int toRead = min(avail, min(remaining, 1024));
                char buf[1025];
                int rd = stream->readBytes((uint8_t*)buf, toRead);
                if (rd <= 0) { delay(IDLE_MS); yield(); continue; }

                buf[rd] = '\0';
                if (totalReceived + rd <= maxBytes) {
                    body       += buf;
                    totalReceived += rd;
                }
                remaining -= rd;
                yield();
            }

            // ── Consume trailing CRLF after payload ──────────────────────────
            bool crSeen = false;
            while (millis() < deadline) {
                if (!stream->available()) { delay(IDLE_MS); yield(); continue; }
                char c = (char)stream->read();
                if (c == '\r') { crSeen = true; continue; }
                if (c == '\n' && crSeen) break;
                // unexpected char — stop consuming
                break;
            }

            if (totalReceived >= maxBytes) {
                Serial.printf("[ReadBody] maxBytes cap hit at %d\n", totalReceived);
                Serial.flush();
                break;
            }
            yield();
        }

        Serial.printf("[ReadBody] Chunked done: %d bytes  heap=%u\n",
                      totalReceived, ESP.getFreeHeap());
        Serial.flush();
        return body;
    }

    // ── _readFixed ────────────────────────────────────────────────────────────
    // Reads exactly contentLen bytes from a Content-Length response.
    // ──────────────────────────────────────────────────────────────────────────
    static String _readFixed(WiFiClient* stream, HTTPClient& client,
                              int contentLen, int maxBytes) {
        const uint32_t TIMEOUT_MS = 10000;
        const uint32_t IDLE_MS    = 5;

        int    toRead  = min(contentLen, maxBytes);
        String body;
        body.reserve(toRead + 8);

        int      received = 0;
        uint32_t deadline = millis() + TIMEOUT_MS;

        while (received < toRead && millis() < deadline) {
            int avail = stream->available();
            if (avail <= 0) {
                if (!client.connected()) break;
                delay(IDLE_MS); yield(); continue;
            }
            int rd_sz = min(avail, min(1024, toRead - received));
            char buf[1025];
            int rd = stream->readBytes((uint8_t*)buf, rd_sz);
            if (rd <= 0) { delay(IDLE_MS); yield(); continue; }
            buf[rd] = '\0';
            body      += buf;
            received  += rd;
            yield();
        }

        Serial.printf("[ReadBody] Fixed done: %d/%d bytes  heap=%u\n",
                      received, toRead, ESP.getFreeHeap());
        Serial.flush();
        return body;
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

    static JsonArray _findEmployeesArray(DynamicJsonDocument& doc) {
        if (doc.containsKey("employees")) {
            JsonArray arr = doc["employees"].as<JsonArray>();
            if (!arr.isNull()) return arr;
        }
        if (doc.containsKey("data") && doc["data"].is<JsonObject>()) {
            JsonObject data = doc["data"].as<JsonObject>();
            if (data.containsKey("employees")) {
                JsonArray arr = data["employees"].as<JsonArray>();
                if (!arr.isNull()) return arr;
            }
        }
        return JsonArray();
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
    // ══════════════════════════════════════════════════════════════════════════
    bool downloadProfileImage(const String& uid, uint8_t** outBuffer, int* outSize,
                               const String& overridePath = "") {
        if (uid.length() == 0 || !outBuffer || !outSize) return false;

        String url;
        if (overridePath.length() > 0) {
            if (overridePath.startsWith("http://") || overridePath.startsWith("https://"))
                url = overridePath;
            else if (overridePath.startsWith("/"))
                url = serverURL + overridePath;
            else
                url = serverURL + "/" + overridePath;
        } else {
            url = serverURL + "/api/profile/" + uid;
        }

        Serial.println("[IMG] GET " + url);
        Serial.printf("[IMG] heap=%u psram=%u\n", ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.flush();

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

        bool validImage = false;
        if (got >= 4) {
            if (buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF)                                        validImage = true;
            else if (buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47)                 validImage = true;
            else if (buf[0] == 0x47 && buf[1] == 0x49 && buf[2] == 0x46)                                    validImage = true;
            else if (got >= 12 && buf[0]==0x52 && buf[1]==0x49 && buf[8]==0x57 && buf[9]==0x45)             validImage = true;
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
                          const String& clockType,
                          const String& clockTimeStr = "",
                          const String& dateStr      = "") {
        Serial.println("\n[HTTP] 📝 Recording attendance: " + clockType);
        Serial.flush();

        String clockTimeFull = (dateStr.length() > 0 && clockTimeStr.length() > 0)
                               ? (dateStr + " " + clockTimeStr)
                               : clockTimeStr;

        DynamicJsonDocument doc(512);
        doc["employee_uid"] = employeeUid;
        doc["nfc_uid"]      = nfcUid;
        doc["nfc_access"]   = nfcUid;
        doc["device_id"]    = deviceId;
        doc["clock_type"]   = clockType;
        doc["clock_time"]   = clockTimeFull;
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
    // fetchAllEmployees
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
    // ══════════════════════════════════════════════════════════════════════════
    template<typename Callback>
    int fetchAllEmployeesEach(Callback onEmployee) {
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

            Serial.printf("[HTTP] Pre-decrypt heap: %u  psram: %u\n",
                          ESP.getFreeHeap(), ESP.getFreePsram());
            Serial.flush();
            DynamicJsonDocument pageDoc(65536);
            bool ok = decryptServerResponse(decryptor, raw, pageDoc);
            raw = "";
            yield();

            if (!ok) {
                Serial.println("[HTTP] decryptServerResponse failed — stopping");
                Serial.flush();
                break;
            }

            Serial.printf("[HTTP] Page doc memory used: %u bytes\n",
                          (unsigned)pageDoc.memoryUsage());
            Serial.flush();

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

            if (pageCount == 0) break;

            int pageIdx = 0;
            for (JsonObject emp : arr) {
                pageIdx++;

                if (total + pageIdx <= 3) {
                    String dbg;
                    serializeJson(emp, dbg);
                    Serial.println("[HTTP] emp#" + String(total + pageIdx) +
                                   " sample: " +
                                   dbg.substring(0, min(300, (int)dbg.length())));
                    Serial.flush();
                }

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
            if (pageCount < PAGE) break;
            if (offset >= 5000)  break;

            delay(100);
        }

        Serial.printf("[HTTP] ===== fetchAllEmployeesEach DONE: %d =====\n", total);
        Serial.flush();
        return total;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // fetchTodayAttendance
    // MEMORY FIX v4: Two-phase approach that avoids OOM on no-PSRAM devices.
    //
    // Problem: 28KB encrypted body + 192KB doc = ~220KB needed, only ~104KB free.
    //
    // Solution:
    //   PHASE A (encrypted path): decrypt the body string first (frees raw body),
    //     then parse the plaintext using a FILTER doc that keeps only the 6 fields
    //     we need per row — reducing doc size from 192KB to ~12KB for 50 records.
    //   PHASE B (plain JSON path): same filter applied directly to raw body string,
    //     then free the string before touching the doc.
    //
    // The filter document itself is tiny (< 512 bytes).
    // ══════════════════════════════════════════════════════════════════════════
    // ══════════════════════════════════════════════════════════════════════════
    // fetchTodayAttendance  — PAGINATED v2
    //
    // Root cause of OOM / "Cannot find closing quote" errors:
    //   The original version fetched ALL attendance rows for the day in a single
    //   HTTP call.  With 105+ rows the encrypted body grew to 49 KB which:
    //     a) Exceeded readHttpBodyReliable's cap → truncated mid-JSON.
    //     b) Consumed all available heap after decryption (~50 KB plain).
    //
    // Fix: request BATCH_SIZE rows per HTTP call and process each page
    //   immediately before fetching the next.  Each page is ~5–8 KB encrypted
    //   (~3–5 KB decrypted) — well within the ESP32's heap budget.
    //
    // The outer seen-cache persists across pages so we never write duplicate
    // CSV rows even when the same employee appears on multiple pages.
    // ══════════════════════════════════════════════════════════════════════════
    int fetchTodayAttendance(const String& dateStr) {
        if (dateStr.length() < 10) {
            SDLogger::log("TodaySync", SDLogger::WARN, "Skipped — no valid date");
            return 0;
        }

        // ── Per-page batch size ───────────────────────────────────────────────
        // 20 rows × ~460 bytes/row (encrypted) ≈ 9 KB per page.
        // Decrypted + parsed with filter ≈ 3–4 KB doc.  Safe on 160 KB heap.
        const int BATCH_SIZE = 20;

        SDLogger::log("TodaySync", SDLogger::INFO,
                      "=== fetchTodayAttendance PAGED START date=" + dateStr + " ===");

        // ── SD seen-cache: persists across pages ─────────────────────────────
        const int MAX_EMP_SEEN = 128;
        String* seenEmpUid   = new String[MAX_EMP_SEEN];
        String* seenCsvTypes = new String[MAX_EMP_SEEN];
        if (!seenEmpUid || !seenCsvTypes) {
            SDLogger::log("TodaySync", SDLogger::ERROR, "OOM seenEmpUid alloc");
            delete[] seenEmpUid; delete[] seenCsvTypes;
            return 0;
        }
        int seenCount = 0;

        auto getCsvTypes = [&](const String& empUid) -> String& {
            for (int i = 0; i < seenCount; i++)
                if (seenEmpUid[i] == empUid) return seenCsvTypes[i];
            if (seenCount < MAX_EMP_SEEN) {
                seenEmpUid[seenCount]   = empUid;
                seenCsvTypes[seenCount] = SDDatabase::isReady()
                                          ? SDDatabase::loadAttendanceToday(empUid)
                                          : "";
                return seenCsvTypes[seenCount++];
            }
            seenCsvTypes[MAX_EMP_SEEN - 1] = SDDatabase::isReady()
                                              ? SDDatabase::loadAttendanceToday(empUid)
                                              : "";
            return seenCsvTypes[MAX_EMP_SEEN - 1];
        };

        // ── ArduinoJson filter — only fields needed per raw attendance row ────
        // The /api/attendance endpoint returns a DOUBLE envelope:
        //   { "data": { "data": [...rows...], "pagination":{} } }
        // Filter must mirror this shape so ArduinoJson reaches the inner array.
        StaticJsonDocument<768> filter;
        JsonObject rowFilter = filter["data"]["data"].createNestedObject();
        rowFilter["employee_uid"] = true;
        rowFilter["clock_type"]   = true;
        rowFilter["clock_time"]   = true;
        rowFilter["id"]           = true;
        rowFilter["first_name"]   = true;
        rowFilter["last_name"]    = true;
        rowFilter["employee_name"]= true;
        rowFilter["department"]   = true;

        int totalSeeded  = 0;
        int totalSkipped = 0;
        int totalSrvIds  = 0;
        int totalRows    = 0;
        int offset       = 0;
        int pageNum      = 0;

        while (true) {
            pageNum++;
            String url = serverURL + "/api/attendance?date=" + dateStr
                         + "&limit=" + String(BATCH_SIZE)
                         + "&offset=" + String(offset)
                         + "&sort_by=clock_time&sort_order=ASC";

            SDLogger::logf("TodaySync", SDLogger::INFO,
                           "Page %d: GET offset=%d  heap=%u",
                           pageNum, offset, ESP.getFreeHeap());

            HTTPClient todayHttp;
            todayHttp.setTimeout(12000);
            todayHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            todayHttp.begin(url);
            todayHttp.addHeader("X-Client-Type", "ESP32");
            if (authToken.length() > 0)
                todayHttp.addHeader("Authorization", "Bearer " + authToken);

            int code = todayHttp.GET();
            SDLogger::logf("TodaySync", SDLogger::INFO,
                           "Page %d HTTP code: %d", pageNum, code);

            if (code <= 0) {
                SDLogger::logf("TodaySync", SDLogger::ERROR,
                               "Page %d: connection error %d — aborting", pageNum, code);
                todayHttp.end();
                break;
            }
            if (code != 200 && code != 403) {
                SDLogger::logf("TodaySync", SDLogger::ERROR,
                               "Page %d: bad HTTP %d — aborting", pageNum, code);
                todayHttp.end();
                break;
            }

            // 12 KB cap: 20 rows × ~460 bytes encrypted + JSON overhead
            String raw = readHttpBodyReliable(todayHttp, 12288);
            todayHttp.end();
            yield();

            int bodyLen = raw.length();
            SDLogger::logf("TodaySync", SDLogger::INFO,
                           "Page %d: body=%d bytes  heap=%u",
                           pageNum, bodyLen, ESP.getFreeHeap());

            if (bodyLen == 0) {
                SDLogger::logf("TodaySync", SDLogger::ERROR,
                               "Page %d: empty body — aborting", pageNum);
                break;
            }

            SDLogger::logf("TodaySync", SDLogger::INFO,
                           "Page %d body[0..80]: %.80s", pageNum, raw.c_str());

            // ── Detect encryption ────────────────────────────────────────────
            bool isEncrypted = (raw.indexOf("\"encrypted\":true") >= 0) &&
                               (raw.indexOf("\"data\":\"") >= 0);

            String plainJson = "";
            if (isEncrypted) {
                int dataKeyIdx = raw.indexOf("\"data\":\"");
                int dataStart  = dataKeyIdx + 8;
                int dataEnd    = raw.indexOf("\"", dataStart);
                if (dataEnd <= dataStart) {
                    SDLogger::logf("TodaySync", SDLogger::ERROR,
                                   "Page %d: cannot find closing quote — aborting", pageNum);
                    break;
                }
                String encPayload = raw.substring(dataStart, dataEnd);
                encPayload.replace("\\/", "/");
                raw = ""; yield();
                SDLogger::logf("TodaySync", SDLogger::INFO,
                               "Page %d: decrypting len=%d  heap=%u",
                               pageNum, (int)encPayload.length(), ESP.getFreeHeap());
                plainJson = decryptor.decrypt(encPayload);
                if (plainJson.length() == 0) {
                    SDLogger::logf("TodaySync", SDLogger::ERROR,
                                   "Page %d: decrypt failed", pageNum);
                    break;
                }
                SDLogger::logf("TodaySync", SDLogger::INFO,
                               "Page %d: decrypted len=%d  heap=%u",
                               pageNum, (int)plainJson.length(), ESP.getFreeHeap());
            } else {
                plainJson = raw;
                raw = ""; yield();
            }

            // ── Parse with filter ────────────────────────────────────────────
            // 20 filtered rows × ~180 bytes each ≈ 3.6 KB + overhead → 8 KB doc
            DynamicJsonDocument* docPtr = new DynamicJsonDocument(12288);
            if (!docPtr) {
                SDLogger::logf("TodaySync", SDLogger::ERROR,
                               "Page %d: OOM doc alloc  heap=%u",
                               pageNum, ESP.getFreeHeap());
                break;
            }

            DeserializationError parseErr = deserializeJson(*docPtr, plainJson,
                                                            DeserializationOption::Filter(filter));
            plainJson = ""; yield();

            if (parseErr) {
                SDLogger::logf("TodaySync", SDLogger::ERROR,
                               "Page %d: parse error: %s  doc=%u  heap=%u",
                               pageNum, parseErr.c_str(),
                               (unsigned)docPtr->memoryUsage(), ESP.getFreeHeap());
                delete docPtr;
                break;
            }

            SDLogger::logf("TodaySync", SDLogger::INFO,
                           "Page %d: parse OK  doc=%u  heap=%u",
                           pageNum, (unsigned)docPtr->memoryUsage(), ESP.getFreeHeap());

            // ── Unwrap data array (double envelope: data.data[]) ─────────────
            JsonArray records;
            if (docPtr->containsKey("data")) {
                auto dataVal = (*docPtr)["data"];
                if (dataVal.is<JsonArray>()) {
                    records = dataVal.as<JsonArray>();
                } else if (dataVal.is<JsonObject>()) {
                    JsonObject dataObj = dataVal.as<JsonObject>();
                    if (dataObj.containsKey("data"))
                        records = dataObj["data"].as<JsonArray>();
                }
            }

            if (records.isNull()) {
                SDLogger::logf("TodaySync", SDLogger::ERROR,
                               "Page %d: no 'data' array found", pageNum);
                delete docPtr;
                break;
            }

            int pageCount = records.size();
            SDLogger::logf("TodaySync", SDLogger::INFO,
                           "Page %d: %d rows  total_so_far=%d",
                           pageNum, pageCount, totalRows + pageCount);

            if (pageCount == 0) {
                delete docPtr;
                break;  // no more data
            }

            totalRows += pageCount;

            // ── Process rows: dedup then write to SD ─────────────────────────
            if (SDDatabase::isReady()) {
                for (JsonObject row : records) {
                    String empUid = "";
                    JsonVariantConst uv = row["employee_uid"];
                    if (uv.is<int>())       empUid = String(uv.as<int>());
                    else if (uv.is<long>()) empUid = String(uv.as<long>());
                    else                    empUid = uv | "";

                    String clockType = row["clock_type"] | "";
                    String clockTime = row["clock_time"] | "";
                    int    srvId     = row["id"]         | 0;

                    if (empUid.length() == 0 || clockType.length() == 0) continue;
                    if (clockType == "absent") continue;

                    // time-only portion for SD key
                    String timeOnly = clockTime;
                    int sp = timeOnly.indexOf(' ');
                    if (sp >= 0) timeOnly = timeOnly.substring(sp + 1);

                    // ── Dedup: skip if already in SD CSV ─────────────────────
                    String& existing = getCsvTypes(empUid);
                    if (("," + existing + ",").indexOf("," + clockType + ",") >= 0) {
                        // Still save server_id mapping if missing
                        if (srvId > 0) {
                            int eid = SDDatabase::getServerIdForRecord(
                                          dateStr, empUid, clockType, timeOnly);
                            if (eid == 0) {
                                SDDatabase::saveServerIdMapping(
                                    dateStr, empUid, clockType, timeOnly, srvId);
                                totalSrvIds++;
                            }
                        }
                        totalSkipped++;
                        yield(); continue;
                    }

                    // ── Build minimal employee profile for CSV row ────────────
                    EmployeeProfile seedEmp;
                    seedEmp.uid  = empUid;
                    seedEmp.hasData = true;
                    String fn = row["first_name"] | "";
                    String ln = row["last_name"]  | "";
                    String en = row["employee_name"] | "";
                    if (en.length() > 0) seedEmp.fullName = en;
                    else if (fn.length() || ln.length()) {
                        seedEmp.fullName = fn + " " + ln;
                        seedEmp.fullName.trim();
                    } else seedEmp.fullName = empUid;
                    seedEmp.department = row["department"] | "";

                    bool wrote = SDDatabase::logAttendance(
                        timeOnly, "", seedEmp, clockType, "SERVER_SEED");

                    if (wrote) {
                        totalSeeded++;
                        // Update seen-cache so later rows in this page are deduped
                        existing = SDDatabase::loadAttendanceToday(empUid);

                        if (srvId > 0) {
                            bool mapped = SDDatabase::saveServerIdMapping(
                                              dateStr, empUid, clockType, timeOnly, srvId);
                            if (mapped) totalSrvIds++;
                        }
                    } else {
                        SDLogger::logf("TodaySync", SDLogger::ERROR,
                                       "CSV WRITE FAILED uid=%s %s @ %s",
                                       empUid.c_str(), clockType.c_str(), timeOnly.c_str());
                    }
                    yield();
                }
            }

            delete docPtr;
            docPtr = nullptr;

            // ── Check pagination ─────────────────────────────────────────────
            offset += pageCount;
            if (pageCount < BATCH_SIZE) break;  // last page
            if (offset >= 2000) {
                SDLogger::log("TodaySync", SDLogger::WARN,
                              "Safety cap: 2000 rows reached — stopping");
                break;
            }

            delay(80);  // brief pause between pages to let TCP close cleanly
            yield();
        }

        delete[] seenEmpUid;
        delete[] seenCsvTypes;

        SDLogger::logf("TodaySync", SDLogger::INFO,
                       "=== DONE: pages=%d seeded=%d skipped=%d srvIds=%d"
                       " (server_rows=%d) heap=%u ===",
                       pageNum, totalSeeded, totalSkipped, totalSrvIds,
                       totalRows, ESP.getFreeHeap());
        Serial.printf("[TodaySync] Done: pages=%d seeded=%d skipped=%d"
                      " (total_rows=%d) heap=%u\n",
                      pageNum, totalSeeded, totalSkipped, totalRows,
                      ESP.getFreeHeap());
        Serial.flush();
        return totalSeeded;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // fetchTodayAttendanceEsp32  — PAGINATED v5
    //
    // Root causes fixed:
    //   1. Chunked encoding corruption ("4000 {...}" body prefix):
    //      readHttpBodyReliable() now detects Content-Length=-1 and calls
    //      _readChunked() which parses hex chunk-size headers properly.
    //   2. Response too large for heap (41KB, no PSRAM):
    //      Requests PAGE_SIZE=10 employees per HTTP call (~3-5KB each).
    //      Iterates pages until total == 0 or page < PAGE_SIZE.
    //
    // Each page is parsed, SD-seeded, and freed before the next page is fetched
    // — peak heap usage is ~20KB instead of 104KB+ for the full batch.
    // ══════════════════════════════════════════════════════════════════════════
    int fetchTodayAttendanceEsp32(const String& date) {
    if (date.length() < 10) {
        SDLogger::log("Esp32Sync", SDLogger::WARN, "Skipped — no valid date (clock not synced yet)");
        return 0;
    }

    SDLogger::log("Esp32Sync", SDLogger::INFO,
                  "=== fetchTodayAttendanceEsp32 PAGED START date=" + date + " ===");

    static const char* SESSION_COLS[] = {
        "morning_in",   "morning_out",
        "afternoon_in", "afternoon_out",
        "evening_in",   "evening_out",
        "overtime_in",  "overtime_out",
        nullptr
    };

    const int MAX_EMP_SEEN = 128;
    String* seenEmpUid   = new String[MAX_EMP_SEEN];
    String* seenCsvTypes = new String[MAX_EMP_SEEN];
    if (!seenEmpUid || !seenCsvTypes) {
        SDLogger::log("Esp32Sync", SDLogger::ERROR, "OOM seenEmpUid");
        delete[] seenEmpUid; delete[] seenCsvTypes;
        return -1;
    }
    int seenCount = 0;

    auto getCsvTypes = [&](const String& empUid) -> String& {
        for (int i = 0; i < seenCount; i++)
            if (seenEmpUid[i] == empUid) return seenCsvTypes[i];
        if (seenCount < MAX_EMP_SEEN) {
            seenEmpUid[seenCount]   = empUid;
            seenCsvTypes[seenCount] = SDDatabase::loadAttendanceToday(empUid);
            return seenCsvTypes[seenCount++];
        }
        seenCsvTypes[MAX_EMP_SEEN - 1] = SDDatabase::loadAttendanceToday(empUid);
        return seenCsvTypes[MAX_EMP_SEEN - 1];
    };

    const int PAGE_SIZE  = 10;
    const int MAX_SEED   = 128;

    struct SeedEntry { String empUid, col, timeOnly, empName, dept; };
    SeedEntry* seedQueue = new SeedEntry[MAX_SEED];
    if (!seedQueue) {
        SDLogger::log("Esp32Sync", SDLogger::ERROR, "OOM seedQueue");
        delete[] seenEmpUid; delete[] seenCsvTypes;
        return -1;
    }

    // ── JSON filter ───────────────────────────────────────────────────────────
    StaticJsonDocument<768> filter;
    JsonObject rowF = filter["data"].createNestedObject();
    rowF["employee_uid"]  = true;
    rowF["employee_name"] = true;
    rowF["first_name"]    = true;
    rowF["last_name"]     = true;
    rowF["department"]    = true;
    rowF["morning_in"]    = true;
    rowF["morning_out"]   = true;
    rowF["afternoon_in"]  = true;
    rowF["afternoon_out"] = true;
    rowF["evening_in"]    = true;
    rowF["evening_out"]   = true;
    rowF["overtime_in"]   = true;
    rowF["overtime_out"]  = true;
    rowF["regular_hours"] = true;
    rowF["overtime_hours"]= true;
    rowF["total_hours"]   = true;

    // ── DEBUG: Print filter structure ─────────────────────────────────────────
    {
        String filterDbg;
        serializeJson(filter, filterDbg);
        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Filter: %.250s", filterDbg.c_str());
    }

    // ── Snapshot file ─────────────────────────────────────────────────────────
    String snapPath = "/attendance/esp32_" + date + ".json";
    File snapFile = SD_MMC.open(snapPath.c_str(), FILE_WRITE);
    if (snapFile) { snapFile.print("["); }
    bool snapFirst = true;

    int totalSeeded   = 0;
    int totalReceived = 0;
    int offset        = 0;
    int pageNum       = 0;
    bool sdReady      = SDDatabase::isReady();

    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                   "sdReady=%d  snapFileOpen=%d",
                   (int)sdReady, (int)(bool)snapFile);

    while (true) {
        pageNum++;
        String url = serverURL + "/api/attendance/esp32-sync?date=" + date
                     + "&limit=" + String(PAGE_SIZE)
                     + "&offset=" + String(offset);

        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d: GET offset=%d  heap=%u",
                       pageNum, offset, ESP.getFreeHeap());

        HTTPClient esp32Http;
        esp32Http.setTimeout(12000);
        esp32Http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        esp32Http.begin(url);
        esp32Http.addHeader("X-Client-Type", "ESP32");
        if (authToken.length() > 0)
            esp32Http.addHeader("Authorization", "Bearer " + authToken);

        int code = esp32Http.GET();
        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d HTTP code: %d", pageNum, code);

        if (code <= 0) {
            SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                           "Page %d: connection error %d — aborting", pageNum, code);
            esp32Http.end();
            break;
        }
        if (code != 200 && code != 403) {
            SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                           "Page %d: bad HTTP code %d — aborting", pageNum, code);
            esp32Http.end();
            break;
        }

        String raw = readHttpBodyReliable(esp32Http, 16384);
        esp32Http.end();
        yield();

        int bodyLen = raw.length();
        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d: body=%d bytes  heap=%u",
                       pageNum, bodyLen, ESP.getFreeHeap());

        if (bodyLen == 0) {
            SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                           "Page %d: empty body — aborting", pageNum);
            break;
        }

        // ── DEBUG 1: Full raw body preview (first 300 chars) ─────────────────
        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d raw[0..300]: %.300s", pageNum, raw.c_str());

        // ── DEBUG 2: Encryption detection details ─────────────────────────────
        bool hasEncFlag   = (raw.indexOf("\"encrypted\":true") >= 0);
        bool hasBase64    = (raw.indexOf("\"data\":\"") >= 0);
        bool isEncrypted  = hasEncFlag && hasBase64;
        char firstChar    = raw.length() > 0 ? raw[0] : '?';
        int  encFlagPos   = raw.indexOf("\"encrypted\":true");
        int  dataQuotePos = raw.indexOf("\"data\":\"");
        int  dataArrPos   = raw.indexOf("\"data\":[");
        int  dataObjPos   = raw.indexOf("\"data\":{");

        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d detect: isEnc=%d encFlag=%d base64=%d firstChar='%c'",
                       pageNum, (int)isEncrypted, (int)hasEncFlag, (int)hasBase64, firstChar);
        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d positions: encFlagPos=%d dataQuotePos=%d dataArrPos=%d dataObjPos=%d",
                       pageNum, encFlagPos, dataQuotePos, dataArrPos, dataObjPos);

        String plainJson = "";

        if (isEncrypted) {
            int dataKeyIdx = raw.indexOf("\"data\":\"");
            int dataStart  = dataKeyIdx + 8;
            int dataEnd    = raw.indexOf("\"", dataStart);

            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                           "Page %d enc: dataKeyIdx=%d dataStart=%d dataEnd=%d",
                           pageNum, dataKeyIdx, dataStart, dataEnd);

            if (dataEnd <= dataStart) {
                SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                               "Page %d: cannot find closing quote — aborting", pageNum);
                break;
            }

            String encPayload = raw.substring(dataStart, dataEnd);
            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                           "Page %d: encPayload len=%d  first40=%.40s",
                           pageNum, (int)encPayload.length(), encPayload.c_str());

            encPayload.replace("\\/", "/");
            raw = "";  yield();

            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                           "Page %d: decrypting  heap=%u", pageNum, ESP.getFreeHeap());
            plainJson = decryptor.decrypt(encPayload);

            if (plainJson.length() == 0) {
                SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                               "Page %d: decrypt failed", pageNum);
                break;
            }

            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                           "Page %d: decrypted len=%d  heap=%u  first100=%.100s",
                           pageNum, (int)plainJson.length(),
                           ESP.getFreeHeap(), plainJson.c_str());
        } else {
            // ── DEBUG 3: Plain JSON path — show what we got ───────────────────
            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                           "Page %d: plain JSON path  len=%d  first100=%.100s",
                           pageNum, bodyLen, raw.c_str());
            plainJson = raw;
            raw = "";
            yield();
        }

        // ── DEBUG 4: plainJson preview before parse ───────────────────────────
        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d: plainJson len=%d  first200=%.200s",
                       pageNum, (int)plainJson.length(), plainJson.c_str());

        // ── Parse WITHOUT filter first to see what's actually there ───────────
        {
            DynamicJsonDocument* probeDoc = new DynamicJsonDocument(16384);
            if (probeDoc) {
                DeserializationError probeErr = deserializeJson(*probeDoc, plainJson);
                SDLogger::logf("Esp32Sync", SDLogger::INFO,
                               "Page %d PROBE (no filter): err=%s  docUsed=%u  heap=%u",
                               pageNum,
                               probeErr ? probeErr.c_str() : "OK",
                               (unsigned)probeDoc->memoryUsage(),
                               ESP.getFreeHeap());

                if (!probeErr) {
                    // Show top-level keys
                    String topKeys = "";
                    for (JsonPair kv : probeDoc->as<JsonObject>())
                        topKeys += String(kv.key().c_str()) + "(" +
                                   String(kv.value().is<JsonArray>()  ? "arr" :
                                          kv.value().is<JsonObject>() ? "obj" :
                                          kv.value().is<bool>()       ? "bool" :
                                          kv.value().is<int>()        ? "int" :
                                          kv.value().is<float>()      ? "float" : "str") + ") ";
                    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                   "Page %d PROBE top keys: %s", pageNum, topKeys.c_str());

                    // Show data array info
                    if (probeDoc->containsKey("data") && (*probeDoc)["data"].is<JsonArray>()) {
                        JsonArray probeArr = (*probeDoc)["data"].as<JsonArray>();
                        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                       "Page %d PROBE data[]: size=%d",
                                       pageNum, (int)probeArr.size());

                        // Show first row's keys and values
                        if (probeArr.size() > 0) {
                            JsonObject firstRow = probeArr[0];
                            String rowKeys = "";
                            for (JsonPair kv : firstRow)
                                rowKeys += String(kv.key().c_str()) + " ";
                            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                           "Page %d PROBE row[0] keys: %s",
                                           pageNum, rowKeys.c_str());

                            // Show full first row serialized
                            String firstRowStr;
                            serializeJson(firstRow, firstRowStr);
                            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                           "Page %d PROBE row[0] data: %.300s",
                                           pageNum, firstRowStr.c_str());
                        }
                    } else if (probeDoc->containsKey("data")) {
                        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                       "Page %d PROBE: 'data' is NOT an array — type check needed",
                                       pageNum);
                    } else {
                        SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                                       "Page %d PROBE: no 'data' key found at all", pageNum);
                    }
                }
                delete probeDoc;
                yield();
            }
        }

        // ── Parse WITH filter ─────────────────────────────────────────────────
        DynamicJsonDocument* docPtr = new DynamicJsonDocument(12288);
        if (!docPtr) {
            SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                           "Page %d: OOM doc alloc  heap=%u",
                           pageNum, ESP.getFreeHeap());
            break;
        }

        DeserializationError parseErr = deserializeJson(*docPtr, plainJson,
                                                        DeserializationOption::Filter(filter));
        plainJson = "";  yield();

        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d FILTERED: err=%s  docUsed=%u / %u  heap=%u",
                       pageNum,
                       parseErr ? parseErr.c_str() : "OK",
                       (unsigned)docPtr->memoryUsage(),
                       (unsigned)docPtr->capacity(),
                       ESP.getFreeHeap());

        if (parseErr) {
            SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                           "Page %d: parse error: %s", pageNum, parseErr.c_str());
            delete docPtr;
            break;
        }

        // ── DEBUG 5: Show filtered result ─────────────────────────────────────
        {
            String filtKeys = "";
            for (JsonPair kv : docPtr->as<JsonObject>())
                filtKeys += String(kv.key().c_str()) + " ";
            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                           "Page %d FILTERED top keys: %s", pageNum, filtKeys.c_str());

            if (docPtr->containsKey("data") && (*docPtr)["data"].is<JsonArray>()) {
                JsonArray filtArr = (*docPtr)["data"].as<JsonArray>();
                SDLogger::logf("Esp32Sync", SDLogger::INFO,
                               "Page %d FILTERED data[]: size=%d", pageNum, (int)filtArr.size());

                if (filtArr.size() > 0) {
                    String filtRowStr;
                    serializeJson(filtArr[0], filtRowStr);
                    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                   "Page %d FILTERED row[0]: %.300s",
                                   pageNum, filtRowStr.c_str());
                }
            } else {
                SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                               "Page %d FILTERED: no 'data' array after filter", pageNum);
            }
        }

        // ── Unwrap data array ─────────────────────────────────────────────────
        JsonArray rows;
        if (docPtr->containsKey("data")) {
            if ((*docPtr)["data"].is<JsonArray>()) {
                rows = (*docPtr)["data"].as<JsonArray>();
                SDLogger::logf("Esp32Sync", SDLogger::INFO,
                               "Page %d: unwrap OK — flat array rows=%d", pageNum, (int)rows.size());
            } else if ((*docPtr)["data"].is<JsonObject>()) {
                JsonObject d = (*docPtr)["data"].as<JsonObject>();
                if (d.containsKey("data") && d["data"].is<JsonArray>()) {
                    rows = d["data"].as<JsonArray>();
                    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                   "Page %d: unwrap OK — double envelope rows=%d",
                                   pageNum, (int)rows.size());
                } else {
                    SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                                   "Page %d: data is object but no inner data[] array", pageNum);
                }
            } else {
                SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                               "Page %d: data key exists but is neither array nor object", pageNum);
            }
        } else {
            SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                           "Page %d: no 'data' key in filtered doc", pageNum);
        }

        if (rows.isNull()) {
            SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                           "Page %d: rows is null — stopping", pageNum);
            delete docPtr;
            break;
        }

        int pageCount = rows.size();
        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d: %d rows  total_so_far=%d",
                       pageNum, pageCount, totalReceived + pageCount);

        if (pageCount == 0) {
            delete docPtr;
            break;
        }

        totalReceived += pageCount;

        // ── Append to snapshot file ───────────────────────────────────────────
        if (snapFile) {
            for (JsonObject row : rows) {
                if (!snapFirst) snapFile.print(",");
                snapFirst = false;

                String empUid = "";
                JsonVariantConst uv = row["employee_uid"];
                if (uv.is<int>())       empUid = String(uv.as<int>());
                else if (uv.is<long>()) empUid = String(uv.as<long>());
                else                    empUid = uv | "";

                // ── DEBUG 6: Show what we're writing to snapshot ──────────────
                if (totalReceived <= 3) {
                    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                   "SnapWrite uid='%s' morning_in='%s' morning_out='%s'",
                                   empUid.c_str(),
                                   (row["morning_in"] | "NULL"),
                                   (row["morning_out"] | "NULL"));
                }

                String empName = row["employee_name"] | "";
                if (empName.length() == 0) {
                    String fn = row["first_name"] | "";
                    String ln = row["last_name"]  | "";
                    if (fn.length() || ln.length()) {
                        empName = fn + " " + ln;
                        empName.trim();
                    }
                }
                String dept = row["department"] | "";

                auto ts = [&](const char* col) -> String {
                    if (row[col].isNull()) return "null";
                    String v = row[col] | "";
                    if (v.length() == 0 || v == "null") return "null";
                    int sp = v.indexOf(' ');
                    if (sp >= 0) v = v.substring(sp + 1);
                    return "\"" + v + "\"";
                };

                snapFile.print("{");
                snapFile.print("\"uid\":\"");        snapFile.print(empUid);  snapFile.print("\",");
                snapFile.print("\"name\":\"");        snapFile.print(empName); snapFile.print("\",");
                snapFile.print("\"dept\":\"");        snapFile.print(dept);    snapFile.print("\",");
                snapFile.print("\"morning_in\":");    snapFile.print(ts("morning_in"));    snapFile.print(",");
                snapFile.print("\"morning_out\":");   snapFile.print(ts("morning_out"));   snapFile.print(",");
                snapFile.print("\"afternoon_in\":");  snapFile.print(ts("afternoon_in"));  snapFile.print(",");
                snapFile.print("\"afternoon_out\":"); snapFile.print(ts("afternoon_out")); snapFile.print(",");
                snapFile.print("\"evening_in\":");    snapFile.print(ts("evening_in"));    snapFile.print(",");
                snapFile.print("\"evening_out\":");   snapFile.print(ts("evening_out"));   snapFile.print(",");
                snapFile.print("\"overtime_in\":");   snapFile.print(ts("overtime_in"));   snapFile.print(",");
                snapFile.print("\"overtime_out\":");  snapFile.print(ts("overtime_out"));  snapFile.print(",");
                snapFile.print("\"reg_h\":");         snapFile.print(row["regular_hours"]  | 0.0f); snapFile.print(",");
                snapFile.print("\"ot_h\":");          snapFile.print(row["overtime_hours"] | 0.0f); snapFile.print(",");
                snapFile.print("\"total_h\":");       snapFile.print(row["total_hours"]    | 0.0f);
                snapFile.print("}");
                yield();
            }
        }

        // ── Collect seed entries ──────────────────────────────────────────────
        if (sdReady) {
            int seedQueueLen = 0;

            for (JsonObject row : rows) {
                String empUid = "";
                JsonVariantConst uv = row["employee_uid"];
                if (uv.is<int>())       empUid = String(uv.as<int>());
                else if (uv.is<long>()) empUid = String(uv.as<long>());
                else                    empUid = uv | "";

                if (empUid.length() == 0) {
                    SDLogger::log("Esp32Sync", SDLogger::WARN, "Skipping row — empty empUid");
                    yield(); continue;
                }

                String empName = row["employee_name"] | "";
                if (empName.length() == 0) {
                    String fn = row["first_name"] | "";
                    String ln = row["last_name"]  | "";
                    if (fn.length() || ln.length()) {
                        empName = fn + " " + ln;
                        empName.trim();
                    }
                }
                if (empName.length() == 0) empName = empUid;
                String dept = row["department"] | "";

                // ── DEBUG 7: Show each employee being processed ───────────────
                SDLogger::logf("Esp32Sync", SDLogger::INFO,
                               "SeedCheck uid=%s name='%s' dept='%s'",
                               empUid.c_str(), empName.c_str(), dept.c_str());

                for (int ci = 0; SESSION_COLS[ci] != nullptr; ci++) {
                    const char* col = SESSION_COLS[ci];

                    // ── DEBUG 8: Show each session column value ───────────────
                    bool colIsNull = row[col].isNull();
                    String colVal  = colIsNull ? "(null)" : (row[col] | "(empty)");
                    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                   "  col=%s  isNull=%d  val='%s'",
                                   col, (int)colIsNull, colVal.c_str());

                    if (colIsNull) { yield(); continue; }

                    String rawTime = row[col] | "";
                    if (rawTime.length() == 0 || rawTime == "null" ||
                        rawTime == "0000-00-00 00:00:00") {
                        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                       "  col=%s SKIP — empty/null rawTime", col);
                        yield(); continue;
                    }

                    String timeOnly = rawTime;
                    int sp = timeOnly.indexOf(' ');
                    if (sp >= 0) timeOnly = timeOnly.substring(sp + 1);

                    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                   "  col=%s rawTime='%s' timeOnly='%s'",
                                   col, rawTime.c_str(), timeOnly.c_str());

                    if (timeOnly.length() == 0) {
                        SDLogger::logf("Esp32Sync", SDLogger::WARN,
                                       "  col=%s SKIP — timeOnly empty after strip", col);
                        yield(); continue;
                    }

                    String& existingTypes = getCsvTypes(empUid);
                    bool alreadyInCsv = (("," + existingTypes + ",").indexOf("," + String(col) + ",") >= 0);

                    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                   "  col=%s existingCSV='%s' alreadyIn=%d",
                                   col, existingTypes.c_str(), (int)alreadyInCsv);

                    if (alreadyInCsv) { yield(); continue; }

                    if (seedQueueLen < MAX_SEED) {
                        seedQueue[seedQueueLen++] = {empUid, String(col), timeOnly, empName, dept};
                        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                                       "  col=%s QUEUED for seed @ %s",
                                       col, timeOnly.c_str());
                    }
                    yield();
                }
                yield();
            }

            SDLogger::logf("Esp32Sync", SDLogger::INFO,
                           "Page %d: seedQueueLen=%d", pageNum, seedQueueLen);

            // ── Sort seed queue by time ───────────────────────────────────────
            for (int i = 1; i < seedQueueLen; i++) {
                SeedEntry e = seedQueue[i];
                int j = i - 1;
                while (j >= 0 && seedQueue[j].timeOnly > e.timeOnly) {
                    seedQueue[j + 1] = seedQueue[j]; j--;
                }
                seedQueue[j + 1] = e;
            }

            // ── Write seed entries to SD ──────────────────────────────────────
            for (int qi = 0; qi < seedQueueLen; qi++) {
                const SeedEntry& e = seedQueue[qi];
                String& existingTypes = getCsvTypes(e.empUid);
                if (("," + existingTypes + ",").indexOf("," + e.col + ",") >= 0) {
                    yield(); continue;
                }

                EmployeeProfile seedEmp;
                seedEmp.uid        = e.empUid;
                seedEmp.fullName   = e.empName;
                seedEmp.department = e.dept;
                seedEmp.hasData    = true;

                SDLogger::logf("Esp32Sync", SDLogger::INFO,
                               "WRITING uid=%s col=%s time=%s name='%s'",
                               e.empUid.c_str(), e.col.c_str(),
                               e.timeOnly.c_str(), e.empName.c_str());

                bool wrote = SDDatabase::logAttendance(e.timeOnly, "", seedEmp, e.col, "SERVER_SEED");

                SDLogger::logf("Esp32Sync", SDLogger::INFO,
                               "WRITE result=%s uid=%s col=%s",
                               wrote ? "OK" : "FAIL",
                               e.empUid.c_str(), e.col.c_str());

                if (wrote) {
                    totalSeeded++;
                    existingTypes = SDDatabase::loadAttendanceToday(e.empUid);
                } else {
                    SDLogger::logf("Esp32Sync", SDLogger::ERROR,
                                   "SEED FAIL uid=%s col=%s sdReady=%d",
                                   e.empUid.c_str(), e.col.c_str(),
                                   (int)SDDatabase::isReady());
                }
                yield();
            }
        } else {
            SDLogger::log("Esp32Sync", SDLogger::ERROR,
                          "SD not ready — skipping all seed writes this page");
        }

        delete docPtr;
        docPtr = nullptr;

        SDLogger::logf("Esp32Sync", SDLogger::INFO,
                       "Page %d done: seeded=%d so far  heap=%u",
                       pageNum, totalSeeded, ESP.getFreeHeap());

        offset += pageCount;
        if (pageCount < PAGE_SIZE) break;
        if (pageNum >= 50) break;

        delay(50);
        yield();
    }

    // ── Finalise snapshot ─────────────────────────────────────────────────────
    if (snapFile) {
        snapFile.print("]");
        snapFile.close();
        SDLogger::log("Esp32Sync", SDLogger::INFO,
                      "Snapshot saved: " + snapPath);
    }

    delete[] seedQueue;
    delete[] seenEmpUid;
    delete[] seenCsvTypes;

    SDLogger::logf("Esp32Sync", SDLogger::INFO,
                   "=== DONE: pages=%d received=%d seeded=%d heap=%u ===",
                   pageNum, totalReceived, totalSeeded, ESP.getFreeHeap());
    Serial.printf("[Esp32Sync] Done: %d pages, %d rows received, %d seeded, heap=%u\n",
                  pageNum, totalReceived, totalSeeded, ESP.getFreeHeap());
    Serial.flush();

    if (totalReceived == 0 && pageNum <= 1) return -1;
    return totalSeeded;
}

    // ══════════════════════════════════════════════════════════════════════════
    // fetchAndSeedByEmployeeList  — SD employee-walk seeder
    //
    // PURPOSE:
    //   The esp32-sync endpoint reads from daily_attendance_summary which is
    //   populated by a server-side aggregation job.  When that job hasn't run
    //   (or runs with stale data), all session columns are NULL → seeded=0.
    //   This function bypasses that table entirely by:
    //     1. Scanning /employees/*.json on SD to get all known numeric UIDs
    //     2. For each UID, GET /api/attendance?date=X&employee_uid=Y (raw table)
    //     3. Seeding any missing clock_type rows into the CSV
    //
    // CALL CONDITION:
    //   Call this when fetchTodayAttendanceEsp32() returns 0 seeded rows AND
    //   fetchTodayAttendance() also returned 0 (i.e. both failed to produce CSV
    //   rows despite valid HTTP 200 responses).
    //
    // RETURNS: number of new CSV rows written, or -1 on fatal error.
    // ══════════════════════════════════════════════════════════════════════════
    int fetchAndSeedByEmployeeList(const String& date) {
        if (date.length() < 10) {
            SDLogger::log("EmpWalk", SDLogger::WARN, "Skipped — no valid date");
            return 0;
        }
        if (!SDDatabase::isReady()) {
            SDLogger::log("EmpWalk", SDLogger::WARN, "SD not ready");
            return 0;
        }

        SDLogger::log("EmpWalk", SDLogger::INFO,
                      "=== fetchAndSeedByEmployeeList START date=" + date + " ===");

        // ── Step 1: Collect numeric employee UIDs from /employees/*.json ──────
        // Employee profiles are stored as /employees/<numeric_uid>.json
        // NFC mappings are stored as /employees/nfc_*.json — skip those.
        // sync_meta.json is also there — skip non-numeric filenames.
        const int MAX_UIDS = 256;
        String* uidList = new String[MAX_UIDS];
        if (!uidList) {
            SDLogger::log("EmpWalk", SDLogger::ERROR, "OOM uidList");
            return -1;
        }
        int uidCount = 0;

        File empDir = SD_MMC.open("/employees");
        if (empDir && empDir.isDirectory()) {
            File entry = empDir.openNextFile();
            while (entry && uidCount < MAX_UIDS) {
                if (!entry.isDirectory()) {
                    String fname = String(entry.name());
                    // entry.name() on ESP32 SD_MMC returns just the filename, not full path
                    // Strip path prefix if present
                    int lastSlash = fname.lastIndexOf('/');
                    if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);

                    // Must end in .json, must not start with "nfc_", must be numeric
                    if (fname.endsWith(".json") &&
                        !fname.startsWith("nfc_") &&
                        fname != "sync_meta.json") {
                        String uid = fname.substring(0, fname.length() - 5); // strip .json
                        // Validate: all digits
                        bool allDigits = (uid.length() > 0);
                        for (int c = 0; c < (int)uid.length(); c++) {
                            if (!isDigit(uid[c])) { allDigits = false; break; }
                        }
                        if (allDigits) {
                            uidList[uidCount++] = uid;
                        }
                    }
                }
                entry.close();
                entry = empDir.openNextFile();
                yield();
            }
            empDir.close();
        } else {
            SDLogger::log("EmpWalk", SDLogger::ERROR, "Cannot open /employees dir");
            delete[] uidList;
            return -1;
        }

        SDLogger::logf("EmpWalk", SDLogger::INFO,
                       "Found %d employee UIDs in SD cache", uidCount);
        if (uidCount == 0) {
            delete[] uidList;
            return 0;
        }

        // ── Step 2: Per-employee seen-cache (same pattern as fetchTodayAttendance) ──
        String* seenEmpUid   = new String[uidCount + 1];
        String* seenCsvTypes = new String[uidCount + 1];
        if (!seenEmpUid || !seenCsvTypes) {
            SDLogger::log("EmpWalk", SDLogger::ERROR, "OOM seen arrays");
            delete[] uidList;
            delete[] seenEmpUid;
            delete[] seenCsvTypes;
            return -1;
        }
        int seenCount = 0;

        auto getCsvTypes = [&](const String& empUid) -> String& {
            for (int i = 0; i < seenCount; i++)
                if (seenEmpUid[i] == empUid) return seenCsvTypes[i];
            if (seenCount < uidCount) {
                seenEmpUid[seenCount]   = empUid;
                seenCsvTypes[seenCount] = SDDatabase::loadAttendanceToday(empUid);
                return seenCsvTypes[seenCount++];
            }
            // fallback: reuse last slot
            seenCsvTypes[uidCount] = SDDatabase::loadAttendanceToday(empUid);
            return seenCsvTypes[uidCount];
        };

        // ── JSON filter for the per-employee attendance response ───────────────
        // /api/attendance?date=X&employee_uid=Y returns:
        //   { "data": { "data": [...rows...] } }  or  { "data": [...rows...] }
        StaticJsonDocument<512> filter;
        JsonObject rowFilter = filter["data"]["data"].createNestedArray().createNestedObject();
        rowFilter["employee_uid"] = true;
        rowFilter["clock_type"]   = true;
        rowFilter["clock_time"]   = true;
        rowFilter["id"]           = true;
        rowFilter["first_name"]   = true;
        rowFilter["last_name"]    = true;
        rowFilter["employee_name"]= true;
        rowFilter["department"]   = true;

        // Also handle flat { "data": [...] } envelope
        StaticJsonDocument<512> filterFlat;
        JsonObject rowFilterFlat = filterFlat["data"].createNestedArray().createNestedObject();
        rowFilterFlat["employee_uid"] = true;
        rowFilterFlat["clock_type"]   = true;
        rowFilterFlat["clock_time"]   = true;
        rowFilterFlat["id"]           = true;
        rowFilterFlat["first_name"]   = true;
        rowFilterFlat["last_name"]    = true;
        rowFilterFlat["employee_name"]= true;
        rowFilterFlat["department"]   = true;

        int totalSeeded  = 0;
        int totalSkipped = 0;
        int empProcessed = 0;

        // ── Step 3: Fetch each employee's attendance ──────────────────────────
        for (int ui = 0; ui < uidCount; ui++) {
            const String& empUid = uidList[ui];

            // Check SD first — if already fully clocked (has at least morning_in),
            // we can skip the HTTP call for employees who've clearly tapped today.
            // But we don't skip if they might have more records on the server.
            // So we always check, but skip if BOTH morning_in AND morning_out exist
            // plus afternoon pairs (i.e. a complete day). For simplicity, only skip
            // if the server HTTP call would be redundant: check if *any* type is
            // already in CSV — we still need to check for more.
            // Strategy: fetch all, deduplicate on write.

            String url = serverURL + "/api/attendance?date=" + date
                         + "&employee_uid=" + empUid
                         + "&limit=20&sort_by=clock_time&sort_order=ASC";

            HTTPClient http;
            http.setTimeout(8000);
            http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            http.begin(url);
            http.addHeader("X-Client-Type", "ESP32");
            if (authToken.length() > 0)
                http.addHeader("Authorization", "Bearer " + authToken);

            int code = http.GET();
            if (code != 200) {
                http.end();
                if (code > 0) {
                    SDLogger::logf("EmpWalk", SDLogger::WARN,
                                   "uid=%s HTTP %d — skip", empUid.c_str(), code);
                }
                yield();
                continue;
            }

            // 6KB cap per employee: 20 rows × ~250 bytes + overhead
            String raw = readHttpBodyReliable(http, 6144);
            http.end();
            yield();

            if (raw.length() == 0) {
                yield(); continue;
            }

            // Detect & decrypt
            bool isEncrypted = (raw.indexOf("\"encrypted\":true") >= 0) &&
                               (raw.indexOf("\"data\":\"") >= 0);
            String plainJson = "";
            if (isEncrypted) {
                int dataKeyIdx = raw.indexOf("\"data\":\"");
                int dataStart  = dataKeyIdx + 8;
                int dataEnd    = raw.indexOf("\"", dataStart);
                if (dataEnd <= dataStart) { yield(); continue; }
                String encPayload = raw.substring(dataStart, dataEnd);
                encPayload.replace("\\/", "/");
                raw = ""; yield();
                plainJson = decryptor.decrypt(encPayload);
                if (plainJson.length() == 0) { yield(); continue; }
            } else {
                plainJson = raw; raw = ""; yield();
            }

            // Parse — try nested envelope first, fall back to flat
            DynamicJsonDocument* docPtr = new DynamicJsonDocument(12288);
            if (!docPtr) { yield(); continue; }

            DeserializationError err = deserializeJson(*docPtr, plainJson,
                                                       DeserializationOption::Filter(filter));
            JsonArray records;
            if (!err) {
                if ((*docPtr)["data"].is<JsonObject>() &&
                    (*docPtr)["data"]["data"].is<JsonArray>()) {
                    records = (*docPtr)["data"]["data"].as<JsonArray>();
                }
            }

            // If nested parse didn't yield records, try flat envelope
            if (records.isNull() || records.size() == 0) {
                delete docPtr;
                docPtr = new DynamicJsonDocument(4096);
                if (!docPtr) { yield(); continue; }
                err = deserializeJson(*docPtr, plainJson,
                                      DeserializationOption::Filter(filterFlat));
                if (!err && (*docPtr)["data"].is<JsonArray>()) {
                    records = (*docPtr)["data"].as<JsonArray>();
                }
            }
            plainJson = ""; yield();

            if (records.isNull()) { delete docPtr; yield(); continue; }

            int pageCount = records.size();
            if (pageCount == 0) { delete docPtr; yield(); continue; }

            empProcessed++;

            // ── Write new rows to CSV ─────────────────────────────────────────
            for (JsonObject row : records) {
                String clockType = row["clock_type"] | "";
                String clockTime = row["clock_time"] | "";
                int    srvId     = row["id"]         | 0;

                if (clockType.length() == 0 || clockType == "absent") {
                    yield(); continue;
                }

                // Time-only portion
                String timeOnly = clockTime;
                int sp = timeOnly.indexOf(' ');
                if (sp >= 0) timeOnly = timeOnly.substring(sp + 1);
                if (timeOnly.length() == 0) { yield(); continue; }

                // Dedup
                String& existing = getCsvTypes(empUid);
                if (("," + existing + ",").indexOf("," + clockType + ",") >= 0) {
                    // Already on SD — just save server ID if missing
                    if (srvId > 0 && SDDatabase::getServerIdForRecord(date, empUid, clockType, timeOnly) == 0) {
                        SDDatabase::saveServerIdMapping(date, empUid, clockType, timeOnly, srvId);
                    }
                    totalSkipped++;
                    yield(); continue;
                }

                // Build minimal employee profile for CSV row
                EmployeeProfile seedEmp;
                seedEmp.uid     = empUid;
                seedEmp.hasData = true;
                String fn = row["first_name"]    | "";
                String ln = row["last_name"]     | "";
                String en = row["employee_name"] | "";
                if      (en.length() > 0)           seedEmp.fullName = en;
                else if (fn.length() || ln.length()) {
                    seedEmp.fullName = fn + " " + ln;
                    seedEmp.fullName.trim();
                } else {
                    // Fall back to SD-cached name
                    EmployeeProfile cached;
                    if (SDDatabase::loadEmployeeProfile(empUid, cached))
                        seedEmp.fullName   = cached.fullName;
                    seedEmp.fullName = seedEmp.fullName.length() > 0 ? seedEmp.fullName : empUid;
                }
                seedEmp.department = row["department"] | "";
                if (seedEmp.department.length() == 0) {
                    EmployeeProfile cached;
                    if (SDDatabase::loadEmployeeProfile(empUid, cached))
                        seedEmp.department = cached.department;
                }

                bool wrote = SDDatabase::logAttendance(timeOnly, "", seedEmp, clockType, "SERVER_SEED");
                if (wrote) {
                    totalSeeded++;
                    existing = SDDatabase::loadAttendanceToday(empUid);
                    if (srvId > 0) {
                        SDDatabase::saveServerIdMapping(date, empUid, clockType, timeOnly, srvId);
                    }
                    SDLogger::logf("EmpWalk", SDLogger::INFO,
                                   "SEEDED uid=%s %s @ %s",
                                   empUid.c_str(), clockType.c_str(), timeOnly.c_str());
                } else {
                    SDLogger::logf("EmpWalk", SDLogger::ERROR,
                                   "WRITE FAIL uid=%s %s", empUid.c_str(), clockType.c_str());
                }
                yield();
            }

            delete docPtr; docPtr = nullptr;

            // Brief pause every 10 employees to let TCP stack breathe
            if ((ui % 10) == 9) { delay(100); }
            yield();
        }

        delete[] uidList;
        delete[] seenEmpUid;
        delete[] seenCsvTypes;

        SDLogger::logf("EmpWalk", SDLogger::INFO,
                       "=== DONE: emps_checked=%d emps_with_data=%d seeded=%d skipped=%d ===",
                       uidCount, empProcessed, totalSeeded, totalSkipped);
        Serial.printf("[EmpWalk] Done: %d UIDs checked, %d had data, %d seeded, %d skipped\n",
                      uidCount, empProcessed, totalSeeded, totalSkipped);
        Serial.flush();
        return totalSeeded;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // seedCsvFromRawAttendance
    //
    // PURPOSE:
    //   Reads the snapshot JSON written by fetchTodayAttendanceEsp32()
    //   (/attendance/esp32_YYYY-MM-DD.json) and seeds any missing clock-type
    //   rows into the daily CSV (/attendance/YYYY-MM-DD.csv).
    //
    // WHY THIS EXISTS:
    //   fetchTodayAttendanceEsp32() saves a per-employee summary JSON that holds
    //   every session column (morning_in … overtime_out) in one object per
    //   employee.  That JSON is the most complete attendance picture available
    //   from the server, but it is ONLY written to the JSON snapshot — it does
    //   NOT automatically produce CSV rows.
    //
    //   The CSV is what the rest of the firmware (check-in/out logic, WiFiManager
    //   Attendance tab, hasCheckedInToday, countTodayCheckIns) reads.  If the
    //   CSV is empty while the snapshot is populated (e.g. after a reboot, or
    //   when the raw /api/attendance endpoint returned 0 rows but the esp32-sync
    //   endpoint returned populated summary rows), the live display shows nothing.
    //
    //   seedCsvFromRawAttendance() bridges that gap by:
    //     1. Opening the snapshot JSON for the given date.
    //     2. Iterating every employee object {uid, name, dept, morning_in, ...}.
    //     3. For each non-null session column, calling SDDatabase::logAttendance()
    //        with the same dedup logic used by all other seeders (seen-cache).
    //     4. Updating the server-ID map if the snapshot contained an "id" field.
    //
    // WHEN TO CALL:
    //   After fetchTodayAttendanceEsp32() and fetchTodayAttendance() have both
    //   been attempted and SDDatabase::countTodayCheckIns() == 0 but the snapshot
    //   file exists.  Example from main.cpp:
    //
    //     int seeded = httpService.fetchTodayAttendanceEsp32(dateStr);
    //     if (seeded <= 0)
    //         seeded = httpService.fetchTodayAttendance(dateStr);
    //     if (seeded <= 0)
    //         httpService.seedCsvFromRawAttendance(dateStr);
    //
    // SNAPSHOT FORMAT (written by fetchTodayAttendanceEsp32):
    //   [
    //     {
    //       "uid":"17", "name":"Llovelyn B. Medina", "dept":"Finance",
    //       "morning_in":"10:43:55",  "morning_out":null,
    //       "afternoon_in":null,      "afternoon_out":null,
    //       "evening_in":null,        "evening_out":null,
    //       "overtime_in":null,       "overtime_out":null,
    //       "reg_h":0.0, "ot_h":0.0, "total_h":0.0
    //     }, ...
    //   ]
    //
    // RETURNS: number of new CSV rows written, or -1 on fatal error.
    // ══════════════════════════════════════════════════════════════════════════
    int seedCsvFromRawAttendance(const String& date) {
        if (date.length() < 10) {
            SDLogger::log("SnapSeed", SDLogger::WARN, "Skipped — no valid date");
            return 0;
        }
        if (!SDDatabase::isReady()) {
            SDLogger::log("SnapSeed", SDLogger::WARN, "SD not ready");
            return 0;
        }

        String snapPath = "/attendance/esp32_" + date + ".json";
        if (!SD_MMC.exists(snapPath.c_str())) {
            SDLogger::log("SnapSeed", SDLogger::WARN,
                          "Snapshot not found: " + snapPath);
            return 0;
        }

        SDLogger::log("SnapSeed", SDLogger::INFO,
                      "=== seedCsvFromRawAttendance START date=" + date
                      + "  snap=" + snapPath + " ===");

        File sf = SD_MMC.open(snapPath.c_str(), FILE_READ);
        if (!sf || sf.size() < 3) {
            SDLogger::log("SnapSeed", SDLogger::ERROR,
                          "Cannot open snapshot (size=" +
                          String(sf ? (int)sf.size() : -1) + ")");
            if (sf) sf.close();
            return -1;
        }

        size_t snapSz = sf.size();
        SDLogger::logf("SnapSeed", SDLogger::INFO,
                       "Snapshot size=%u bytes  heap=%u", (unsigned)snapSz,
                       ESP.getFreeHeap());

        // ── Parse the snapshot JSON ───────────────────────────────────────────
        // Each employee object is ~200-250 bytes; 50 employees ≈ 12 KB raw.
        // Allocate 2× the file size (minimum 16 KB) for the parsed doc.
        size_t docCap = max((size_t)16384, snapSz * 2);
       DynamicJsonDocument* snapDoc = new DynamicJsonDocument(docCap);
        if (!snapDoc) {
            SDLogger::logf("SnapSeed", SDLogger::ERROR,
                        "OOM: cannot allocate %u-byte doc  heap=%u",
                        (unsigned)docCap, ESP.getFreeHeap());
            sf.close();
            return -1;
        }
        if (!snapDoc) {
            snapDoc = new DynamicJsonDocument(docCap);
        }
        if (!snapDoc) {
            SDLogger::logf("SnapSeed", SDLogger::ERROR,
                           "OOM: cannot allocate %u-byte doc  heap=%u",
                           (unsigned)docCap, ESP.getFreeHeap());
            sf.close();
            return -1;
        }

        DeserializationError parseErr = deserializeJson(*snapDoc, sf);
        sf.close();
        yield();

        if (parseErr) {
            SDLogger::logf("SnapSeed", SDLogger::ERROR,
                           "JSON parse error: %s", parseErr.c_str());
            delete snapDoc;
            return -1;
        }

        if (!snapDoc->is<JsonArray>()) {
            SDLogger::log("SnapSeed", SDLogger::ERROR,
                          "Snapshot root is not an array");
            delete snapDoc;
            return -1;
        }

        JsonArray empArray = snapDoc->as<JsonArray>();
        int empCount = empArray.size();
        SDLogger::logf("SnapSeed", SDLogger::INFO,
                       "Snapshot has %d employee objects  doc=%u bytes",
                       empCount, (unsigned)snapDoc->memoryUsage());

        if (empCount == 0) {
            delete snapDoc;
            SDLogger::log("SnapSeed", SDLogger::WARN,
                          "Snapshot array is empty — nothing to seed");
            return 0;
        }

        // ── Columns to expand into individual CSV clock-type rows ─────────────
        static const char* SESSION_COLS[] = {
            "morning_in",   "morning_out",
            "afternoon_in", "afternoon_out",
            "evening_in",   "evening_out",
            "overtime_in",  "overtime_out",
            nullptr
        };

        // ── Per-employee seen-cache: avoids re-reading SD for every row ────────
        const int MAX_SEEN = 128;
        String* seenUid   = new String[MAX_SEEN];
        String* seenTypes = new String[MAX_SEEN];
        if (!seenUid || !seenTypes) {
            SDLogger::log("SnapSeed", SDLogger::ERROR, "OOM seen arrays");
            delete[] seenUid; delete[] seenTypes;
            delete snapDoc;
            return -1;
        }
        int seenCount = 0;

        // Returns (by reference) the comma-separated clock_types already in
        // today's CSV for empUid.  Populated lazily from SD on first access.
        auto getCsvTypes = [&](const String& empUid) -> String& {
            for (int i = 0; i < seenCount; i++)
                if (seenUid[i] == empUid) return seenTypes[i];
            if (seenCount < MAX_SEEN) {
                seenUid[seenCount]   = empUid;
                seenTypes[seenCount] = SDDatabase::loadAttendanceToday(empUid);
                return seenTypes[seenCount++];
            }
            // Cache full — reuse the last slot
            seenTypes[MAX_SEEN - 1] = SDDatabase::loadAttendanceToday(empUid);
            return seenTypes[MAX_SEEN - 1];
        };

        int totalSeeded  = 0;
        int totalSkipped = 0;
        int totalNoTime  = 0;

        // ── Iterate every employee object in the snapshot ─────────────────────
        for (JsonObject emp : empArray) {
            // ── Extract employee identity ─────────────────────────────────────
            String empUid = "";
            JsonVariantConst uv = emp["uid"];
            if      (uv.is<int>())  empUid = String(uv.as<int>());
            else if (uv.is<long>()) empUid = String(uv.as<long>());
            else                    empUid = uv | "";
            if (empUid.length() == 0) { yield(); continue; }

            String empName = emp["name"] | "";
            String dept    = emp["dept"] | "";

            // Fall back to SD-cached profile if snapshot name/dept is blank
            if (empName.length() == 0 || dept.length() == 0) {
                EmployeeProfile cached;
                if (SDDatabase::loadEmployeeProfile(empUid, cached)) {
                    if (empName.length() == 0) empName = cached.fullName;
                    if (dept.length() == 0)    dept    = cached.department;
                }
            }
            if (empName.length() == 0) empName = empUid;

            SDLogger::logf("SnapSeed", SDLogger::INFO,
                           "Processing uid=%s name='%s' dept='%s'",
                           empUid.c_str(), empName.c_str(), dept.c_str());

            // ── Iterate session columns ───────────────────────────────────────
            for (int ci = 0; SESSION_COLS[ci] != nullptr; ci++) {
                const char* col = SESSION_COLS[ci];

                // Skip nulls
                if (emp[col].isNull()) { totalNoTime++; yield(); continue; }

                String timeVal = emp[col] | "";
                if (timeVal.length() == 0 || timeVal == "null" ||
                    timeVal == "0000-00-00 00:00:00") {
                    totalNoTime++;
                    yield(); continue;
                }

                // Snapshot writer strips the date prefix so values are already
                // "HH:MM:SS".  Handle the case where it wasn't stripped yet.
                int spIdx = timeVal.indexOf(' ');
                if (spIdx >= 0) timeVal = timeVal.substring(spIdx + 1);
                if (timeVal.length() == 0) { totalNoTime++; yield(); continue; }

                // ── Dedup: skip if this clock_type already exists in today's CSV
                String& existingTypes = getCsvTypes(empUid);
                String  needle        = "," + String(col) + ",";
                if (("," + existingTypes + ",").indexOf(needle) >= 0) {
                    SDLogger::logf("SnapSeed", SDLogger::INFO,
                                   "SKIP  uid=%s %s (already in CSV)",
                                   empUid.c_str(), col);
                    totalSkipped++;
                    yield(); continue;
                }

                // ── Write the row to the daily CSV ────────────────────────────
                EmployeeProfile seedEmp;
                seedEmp.uid        = empUid;
                seedEmp.fullName   = empName;
                seedEmp.department = dept;
                seedEmp.hasData    = true;

                bool wrote = SDDatabase::logAttendance(
                    timeVal,          // timestamp  (HH:MM:SS)
                    "",               // nfcUid     (unknown from snapshot)
                    seedEmp,
                    String(col),      // eventType  e.g. "morning_in"
                    "SNAP_SEED"       // deviceId sentinel
                );

                if (wrote) {
                    totalSeeded++;
                    // Refresh seen-cache so subsequent columns in the same
                    // employee object are correctly deduped without an SD read.
                    existingTypes = SDDatabase::loadAttendanceToday(empUid);

                    SDLogger::logf("SnapSeed", SDLogger::INFO,
                                   "SEEDED uid=%s %s @ %s",
                                   empUid.c_str(), col, timeVal.c_str());
                } else {
                    SDLogger::logf("SnapSeed", SDLogger::ERROR,
                                   "WRITE FAIL uid=%s %s @ %s",
                                   empUid.c_str(), col, timeVal.c_str());
                }
                yield();
            } // end session columns

            yield();
        } // end employee loop

        delete[] seenUid;
        delete[] seenTypes;
        delete snapDoc;

        SDLogger::logf("SnapSeed", SDLogger::INFO,
                       "=== DONE: emps=%d seeded=%d skipped=%d noTime=%d heap=%u ===",
                       empCount, totalSeeded, totalSkipped, totalNoTime,
                       ESP.getFreeHeap());
        Serial.printf("[SnapSeed] Done: %d employees, %d rows seeded,"
                      " %d skipped, %d null, heap=%u\n",
                      empCount, totalSeeded, totalSkipped, totalNoTime,
                      ESP.getFreeHeap());
        Serial.flush();
        return totalSeeded;
    }

    // ══════════════════════════════════════════════════════════════════════════
    // downloadAllPhotosZip
    // ══════════════════════════════════════════════════════════════════════════
    int downloadAllPhotosZip(const String* uidArray, int uidCount,
                              void (*progressCb)(int cur, int total, const String& uid) = nullptr) {
        if (uidCount == 0) return 0;

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
        zipHttp.setTimeout(60000);
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

        const int MAX_ZIP = 10 * 1024 * 1024;
        if (zipSize > MAX_ZIP) {
            Serial.printf("[ZIP] FAILED: ZIP too large (%d B)\n", zipSize);
            zipHttp.end();
            return 0;
        }

        int allocSize = (zipSize > 0) ? (zipSize + 256) : 524288;
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

        if (got < 22) {
            Serial.println("[ZIP] FAILED: data too short to be a ZIP");
            free(zipBuf);
            return 0;
        }

        if (zipBuf[0] != 0x50 || zipBuf[1] != 0x4B) {
            Serial.printf("[ZIP] FAILED: not a ZIP (magic %02X %02X). Preview: ", zipBuf[0], zipBuf[1]);
            char preview[201]; int plen = min(got, 200);
            memcpy(preview, zipBuf, plen); preview[plen] = 0;
            Serial.println(String(preview));
            Serial.flush();
            free(zipBuf);
            return 0;
        }

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

        int saved = 0;
        int pos   = 0;
        int entry = 0;

        while (pos + 30 <= got) {
            uint32_t sig = (uint32_t)zipBuf[pos]        |
                           ((uint32_t)zipBuf[pos + 1] << 8)  |
                           ((uint32_t)zipBuf[pos + 2] << 16) |
                           ((uint32_t)zipBuf[pos + 3] << 24);

            if (sig == 0x02014B50 || sig == 0x06054B50) break;
            if (sig != 0x04034B50) { pos++; continue; }

            uint16_t method      = (uint16_t)zipBuf[pos +  8] | ((uint16_t)zipBuf[pos +  9] << 8);
            uint32_t compSize    = (uint32_t)zipBuf[pos + 18] | ((uint32_t)zipBuf[pos + 19] << 8)
                                 | ((uint32_t)zipBuf[pos + 20] << 16) | ((uint32_t)zipBuf[pos + 21] << 24);
            uint32_t uncompSize  = (uint32_t)zipBuf[pos + 22] | ((uint32_t)zipBuf[pos + 23] << 8)
                                 | ((uint32_t)zipBuf[pos + 24] << 16) | ((uint32_t)zipBuf[pos + 25] << 24);
            uint16_t fnLen       = (uint16_t)zipBuf[pos + 26] | ((uint16_t)zipBuf[pos + 27] << 8);
            uint16_t extraLen    = (uint16_t)zipBuf[pos + 28] | ((uint16_t)zipBuf[pos + 29] << 8);

            int dataOffset = pos + 30 + fnLen + extraLen;

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

            String fnLower = filename; fnLower.toLowerCase();
            bool isImg = fnLower.endsWith(".jpg") || fnLower.endsWith(".jpeg") ||
                         fnLower.endsWith(".png") || fnLower.endsWith(".webp") ||
                         fnLower.endsWith(".gif");

            if (!isImg) {
                Serial.println("[ZIP] Skip non-image: " + filename);
                pos = dataOffset + compSize;
                continue;
            }

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