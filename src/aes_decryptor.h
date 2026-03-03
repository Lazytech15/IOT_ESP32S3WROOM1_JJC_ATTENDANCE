//current project - AES-256-CBC DECRYPTION FOR ESP32
//
// FIX v2:
// - All malloc() calls for decode/plaintext buffers now use ps_malloc() first
//   (PSRAM) and fall back to heap malloc() only if PSRAM is unavailable.
//   This prevents heap OOM when decrypting large paginated responses
//   (50 employees with face_descriptor can produce 60-100 KB of base64
//   ciphertext; the decryptor needs TWO large buffers simultaneously).
// - Added explicit heap+PSRAM logging before and after each allocation
//   so OOM failures are visible in Serial output.

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

    String decrypt(const String& base64Payload) {
        if (!_keyReady) {
            Serial.println("[AES] Cannot decrypt — key not ready");
            Serial.flush();
            return "";
        }

        Serial.printf("[AES] decrypt() called — payload len=%d  heap=%u  psram=%u\n",
                      (int)base64Payload.length(),
                      ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.flush();

        // ── 1. Base64-decode ──────────────────────────────────────────────────
        size_t decodedLen = 0;
        const unsigned char* src = (const unsigned char*)base64Payload.c_str();
        size_t srcLen = base64Payload.length();

        mbedtls_base64_decode(nullptr, 0, &decodedLen, src, srcLen);

        if (decodedLen < 17) {
            Serial.println("[AES] Payload too short: " + String(decodedLen));
            Serial.flush();
            return "";
        }

        Serial.printf("[AES] Decoded size will be %u bytes\n", (unsigned)decodedLen);
        Serial.flush();

        // Use PSRAM-aware alloc for the decode buffer
        uint8_t* decoded = _aes_alloc(decodedLen, "decode-buf");
        if (!decoded) return "";

        size_t actualLen = 0;
        int ret = mbedtls_base64_decode(decoded, decodedLen, &actualLen, src, srcLen);
        if (ret != 0) {
            Serial.println("[AES] Base64 decode failed: " + String(ret));
            Serial.flush();
            free(decoded);
            return "";
        }

        // ── 2. Split IV / ciphertext ──────────────────────────────────────────
        const int IV_LEN = 16;
        if ((int)actualLen <= IV_LEN) {
            Serial.println("[AES] Not enough data after base64 decode");
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
        // Use PSRAM-aware alloc for the plaintext buffer
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
            Serial.println("[AES] Invalid PKCS#7 padding: " + String(padLen));
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
// decryptServerResponse  (free function)
// ══════════════════════════════════════════════════════════════════════════════
inline bool decryptServerResponse(AesDecryptor&        decryptor,
                                  const String&        rawBody,
                                  DynamicJsonDocument& outDoc) {
    bool isEncrypted = (rawBody.indexOf("\"encrypted\":true") >= 0);

    Serial.printf("[AES] decryptServerResponse: bodyLen=%d encrypted=%s\n",
                  (int)rawBody.length(), isEncrypted ? "YES" : "NO");
    Serial.flush();

    if (!isEncrypted) {
        DeserializationError err = deserializeJson(outDoc, rawBody);
        if (err) {
            Serial.print("[AES] Direct JSON parse error: ");
            Serial.println(err.c_str());
            Serial.flush();
            return false;
        }
        return true;
    }

    // ── Extract base64 "data" field by string search (avoids huge JSON parse) ─
    int dataKeyIdx = rawBody.indexOf("\"data\":\"");
    if (dataKeyIdx < 0) {
        Serial.println("[AES] No 'data' field found in encrypted envelope");
        Serial.println("[AES] Body preview: " + rawBody.substring(0, min(200,(int)rawBody.length())));
        Serial.flush();
        return false;
    }

    int dataStart = dataKeyIdx + 8;
    int dataEnd   = rawBody.indexOf("\"", dataStart);
    if (dataEnd < 0) {
        Serial.println("[AES] Could not find closing quote of 'data' field");
        Serial.flush();
        return false;
    }

    String encryptedData = rawBody.substring(dataStart, dataEnd);
    encryptedData.replace("\\/", "/");  // PHP json_encode escapes /

    Serial.printf("[AES] Extracted base64 len: %d  heap: %u  psram: %u\n",
                  (int)encryptedData.length(),
                  ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.flush();

    // ── Decrypt ────────────────────────────────────────────────────────────────
    String plainJson = decryptor.decrypt(encryptedData);
    if (plainJson.length() == 0) {
        Serial.println("[AES] Decryption returned empty string");
        Serial.flush();
        return false;
    }

    Serial.printf("[AES] Plain JSON len: %d\n", (int)plainJson.length());
    Serial.println("[AES] Plain preview: " +
                   plainJson.substring(0, min(200, (int)plainJson.length())));
    Serial.flush();

    // ── Parse into caller's document ──────────────────────────────────────────
    DeserializationError err = deserializeJson(outDoc, plainJson);
    if (err) {
        Serial.print("[AES] Inner JSON parse error: ");
        Serial.println(err.c_str());
        Serial.printf("[AES] outDoc capacity: %u  plainJson len: %d\n",
                      (unsigned)outDoc.capacity(), (int)plainJson.length());
        Serial.flush();
        return false;
    }

    return true;
}