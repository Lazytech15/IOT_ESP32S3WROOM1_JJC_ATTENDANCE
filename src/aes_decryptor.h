// current project - AES-256-CBC DECRYPTION FOR ESP32
//
// FIX v3:
// - decryptServerResponse() now uses a two-pass strategy:
//     PASS 1: If the body contains "encrypted":true AND a base64 "data"
//             string field, attempt AES decrypt.
//     PASS 2: If pass 1 is skipped or fails, attempt direct JSON parse.
//   This makes the function robust against:
//     • Server returning plain JSON (e.g. esp32-sync before server fix)
//     • Any future endpoint where encryption middleware is bypassed
//     • Field-name mismatches (falls back gracefully)
// - Added detailed Serial logging at every decision point so failures
//   are visible without needing a separate body-preview log.
// - All malloc() calls still use ps_malloc() first (PSRAM) with heap
//   fallback, same as v2.

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include "mbedtls/aes.h"
#include "mbedtls/base64.h"

// ── PSRAM-aware malloc helper ─────────────────────────────────────────────────
// Tries PSRAM first (ps_malloc), falls back to regular heap malloc.
// Always logs which allocator was used and whether it succeeded.
static inline uint8_t* _aes_alloc(size_t size, const char* label) {
    uint8_t* ptr = nullptr;
    if (psramFound()) {
        ptr = (uint8_t*)ps_malloc(size);
        if (ptr) {
            Serial.printf("[AES] %s: ps_malloc %u bytes OK\n", label, (unsigned)size);
            return ptr;
        }
        Serial.printf("[AES] %s: ps_malloc %u FAILED, trying heap\n", label, (unsigned)size);
    }
    ptr = (uint8_t*)malloc(size);
    if (ptr) {
        Serial.printf("[AES] %s: heap malloc %u bytes OK\n", label, (unsigned)size);
    } else {
        Serial.printf("[AES] %s: heap malloc %u FAILED — out of memory!\n", label, (unsigned)size);
        Serial.printf("[AES]   Free heap: %u  Free PSRAM: %u\n",
                      ESP.getFreeHeap(), ESP.getFreePsram());
    }
    Serial.flush();
    return ptr;
}

// ══════════════════════════════════════════════════════════════════════════════
// AesDecryptor
// Wraps mbedTLS AES-256-CBC. Key is provided as base64 (same value as PHP
// API_ENCRYPTION_KEY). decrypt() accepts base64( iv || ciphertext ) — the
// format produced by the PHP EncryptionMiddleware::encrypt() method.
// ══════════════════════════════════════════════════════════════════════════════
class AesDecryptor {
public:
    explicit AesDecryptor(const char* base64Key) {
        size_t outLen = 0;
        int ret = mbedtls_base64_decode(
            _key, sizeof(_key), &outLen,
            (const unsigned char*)base64Key, strlen(base64Key)
        );
        _keyReady = (ret == 0 && outLen == 32);
        if (!_keyReady) {
            Serial.println("[AES] ERROR: key decode failed (ret=" +
                           String(ret) + " outLen=" + String(outLen) + ")");
        }
        Serial.flush();
    }

    // ── decrypt ───────────────────────────────────────────────────────────────
    // Input:  base64-encoded  ( 16-byte IV || AES-CBC ciphertext )
    // Output: decrypted plaintext String, or "" on any failure.
    String decrypt(const String& base64Payload) {
        if (!_keyReady) {
            Serial.println("[AES] Cannot decrypt — key not ready");
            Serial.flush();
            return "";
        }

        Serial.printf("[AES] decrypt() — payload len=%d  heap=%u  psram=%u\n",
                      (int)base64Payload.length(),
                      ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.flush();

        // ── 1. Base64-decode ──────────────────────────────────────────────────
        size_t decodedLen = 0;
        const unsigned char* src    = (const unsigned char*)base64Payload.c_str();
        size_t               srcLen = base64Payload.length();

        // First call: get the required buffer size.
        mbedtls_base64_decode(nullptr, 0, &decodedLen, src, srcLen);

        if (decodedLen < 17) {
            Serial.println("[AES] Payload too short after base64 decode: " + String(decodedLen));
            Serial.flush();
            return "";
        }

        Serial.printf("[AES] Decoded size will be %u bytes\n", (unsigned)decodedLen);
        Serial.flush();

        uint8_t* decoded = _aes_alloc(decodedLen, "decode-buf");
        if (!decoded) return "";

        size_t actualLen = 0;
        int ret = mbedtls_base64_decode(decoded, decodedLen, &actualLen, src, srcLen);
        if (ret != 0) {
            Serial.println("[AES] Base64 decode error: " + String(ret));
            Serial.flush();
            free(decoded);
            return "";
        }

        // ── 2. Split IV / ciphertext ──────────────────────────────────────────
        const int IV_LEN = 16;
        if ((int)actualLen <= IV_LEN) {
            Serial.println("[AES] Not enough data for IV + ciphertext: " + String(actualLen));
            Serial.flush();
            free(decoded);
            return "";
        }

        uint8_t iv[IV_LEN];
        memcpy(iv, decoded, IV_LEN);

        const uint8_t* ciphertext    = decoded + IV_LEN;
        size_t         ciphertextLen = actualLen - IV_LEN;

        if (ciphertextLen % 16 != 0) {
            Serial.println("[AES] Ciphertext not block-aligned: " + String(ciphertextLen));
            Serial.flush();
            free(decoded);
            return "";
        }

        // ── 3. AES-256-CBC decrypt ─────────────────────────────────────────────
        uint8_t* plaintext = _aes_alloc(ciphertextLen + 1, "plain-buf");
        if (!plaintext) {
            free(decoded);
            return "";
        }

        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);

        ret = mbedtls_aes_setkey_dec(&aes, _key, 256);
        if (ret != 0) {
            Serial.println("[AES] setkey_dec failed: " + String(ret));
            Serial.flush();
            mbedtls_aes_free(&aes);
            free(decoded);
            free(plaintext);
            return "";
        }

        uint8_t ivCopy[IV_LEN];
        memcpy(ivCopy, iv, IV_LEN);

        ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                                     ciphertextLen, ivCopy,
                                     ciphertext, plaintext);
        mbedtls_aes_free(&aes);
        free(decoded);   // release decode buffer before building String

        if (ret != 0) {
            Serial.println("[AES] CBC decrypt failed: " + String(ret));
            Serial.flush();
            free(plaintext);
            return "";
        }

        // ── 4. Strip PKCS#7 padding ────────────────────────────────────────────
        uint8_t padLen = plaintext[ciphertextLen - 1];
        if (padLen == 0 || padLen > 16) {
            Serial.println("[AES] Invalid PKCS#7 pad byte: " + String(padLen));
            Serial.flush();
            free(plaintext);
            return "";
        }

        bool padOk = true;
        for (int i = 0; i < padLen; i++) {
            if (plaintext[ciphertextLen - 1 - i] != padLen) {
                padOk = false;
                break;
            }
        }
        if (!padOk) {
            Serial.println("[AES] PKCS#7 padding verification failed");
            Serial.flush();
            free(plaintext);
            return "";
        }

        size_t plainLen = ciphertextLen - padLen;
        plaintext[plainLen] = '\0';

        String result = String((char*)plaintext);
        free(plaintext);

        Serial.printf("[AES] Decrypted OK — %u plaintext bytes\n", (unsigned)plainLen);
        Serial.flush();
        return result;
    }

    bool isReady() const { return _keyReady; }

private:
    uint8_t _key[32];
    bool    _keyReady = false;
};


// ══════════════════════════════════════════════════════════════════════════════
// decryptServerResponse  (free function, v3)
//
// Strategy (two-pass):
//
// PASS 1 — Encrypted path
//   Triggered when body contains "encrypted":true AND a quoted "data" field
//   (i.e. "data":"<base64>"). The quoted-value check ensures we only try
//   AES when data is a string, NOT when data is an object/array (plain JSON).
//   On success: outDoc is populated from the decrypted JSON → return true.
//   On failure: fall through to pass 2 (don't give up immediately).
//
// PASS 2 — Plain JSON path
//   Attempt direct deserialisation of the raw body into outDoc.
//   Handles:
//     • Endpoints where encryption middleware was bypassed / not yet fixed.
//     • Future endpoints intentionally left unencrypted.
//   On success: return true.
//   On failure: return false (nothing more we can do).
//
// Callers don't need to know which path succeeded — the populated outDoc
// is identical either way.
// ══════════════════════════════════════════════════════════════════════════════
inline bool decryptServerResponse(AesDecryptor&        decryptor,
                                  const String&        rawBody,
                                  DynamicJsonDocument& outDoc) {

    // ── Diagnostic header ─────────────────────────────────────────────────────
    Serial.printf("[AES] decryptServerResponse: bodyLen=%d  heap=%u  psram=%u\n",
                  (int)rawBody.length(), ESP.getFreeHeap(), ESP.getFreePsram());
    // Print first 200 chars so we can see whether it's an encrypted envelope
    // or plain JSON without adding a separate log call in the caller.
    Serial.println("[AES] Body[0..199]: " +
                   rawBody.substring(0, min(200, (int)rawBody.length())));
    Serial.flush();

    // ── Detect envelope fields ────────────────────────────────────────────────
    // "encrypted":true  → server confirms this is an encrypted response.
    // "data":"          → "data" value is a quoted string (base64), not an
    //                     object/array.  This is the field we decrypt.
    bool hasEncryptedFlag = (rawBody.indexOf("\"encrypted\":true") >= 0);
    // indexOf returns the position of the substring; >= 0 means found.
    // We look for the literal `"data":"` (8 chars incl. opening quote of value)
    // so we don't confuse it with `"data":{` (object) or `"data":[` (array).
    bool hasBase64Data    = (rawBody.indexOf("\"data\":\"") >= 0);

    Serial.printf("[AES] encryptedFlag=%s  base64DataField=%s\n",
                  hasEncryptedFlag ? "YES" : "NO",
                  hasBase64Data    ? "YES" : "NO");
    Serial.flush();

    // ── PASS 1: AES decrypt ───────────────────────────────────────────────────
    if (hasEncryptedFlag && hasBase64Data) {
        Serial.println("[AES] PASS 1: attempting AES decrypt");
        Serial.flush();

        // Extract the base64 ciphertext by string search — avoids allocating
        // a huge DynamicJsonDocument just to pull one field.
        int dataKeyIdx = rawBody.indexOf("\"data\":\"");
        int dataStart  = dataKeyIdx + 8;                      // skip past "data":"
        int dataEnd    = rawBody.indexOf("\"", dataStart);    // closing quote

        if (dataEnd <= dataStart) {
            Serial.println("[AES] PASS 1: could not find closing quote of 'data' — skipping to PASS 2");
            Serial.flush();
            goto pass2;
        }

        {
            String encryptedData = rawBody.substring(dataStart, dataEnd);
            // PHP json_encode escapes forward-slashes; undo that.
            encryptedData.replace("\\/", "/");

            Serial.printf("[AES] PASS 1: base64 len=%d\n", (int)encryptedData.length());
            Serial.flush();

            String plainJson = decryptor.decrypt(encryptedData);

            if (plainJson.length() == 0) {
                Serial.println("[AES] PASS 1: decrypt returned empty — falling through to PASS 2");
                Serial.flush();
                goto pass2;
            }

            Serial.printf("[AES] PASS 1: plaintext len=%d\n", (int)plainJson.length());
            Serial.println("[AES] PASS 1 plain[0..199]: " +
                           plainJson.substring(0, min(200, (int)plainJson.length())));
            Serial.flush();

            outDoc.clear();
            DeserializationError err = deserializeJson(outDoc, plainJson);
            if (!err) {
                Serial.println("[AES] PASS 1: JSON parse OK → success");
                Serial.flush();
                return true;
            }

            Serial.println("[AES] PASS 1: inner JSON parse error: " + String(err.c_str()));
            Serial.printf("[AES] PASS 1: outDoc capacity=%u  plainJson len=%d\n",
                          (unsigned)outDoc.capacity(), (int)plainJson.length());
            Serial.flush();
            // Fall through to PASS 2 — maybe the body itself is also valid JSON.
        }
    } else {
        Serial.println("[AES] PASS 1: skipped (encrypted envelope not detected)");
        Serial.flush();
    }

    // ── PASS 2: plain JSON parse ──────────────────────────────────────────────
    pass2:
    Serial.println("[AES] PASS 2: attempting direct JSON parse");
    Serial.flush();

    outDoc.clear();
    {
        DeserializationError err = deserializeJson(outDoc, rawBody);
        if (!err) {
            Serial.println("[AES] PASS 2: JSON parse OK → success");
            Serial.flush();
            return true;
        }

        Serial.println("[AES] PASS 2: JSON parse error: " + String(err.c_str()));
        Serial.printf("[AES] PASS 2: outDoc capacity=%u  bodyLen=%d\n",
                      (unsigned)outDoc.capacity(), (int)rawBody.length());
        Serial.flush();
    }

    Serial.println("[AES] Both passes failed — returning false");
    Serial.flush();
    return false;
}