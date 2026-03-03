#include "nfc_manager.h"

// ── PN532 instance (Software SPI) ────────────────────────────────────────────
// NOT static — exported via extern in nfc_manager.h so main.cpp can poll it.
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

// ── Public state ──────────────────────────────────────────────────────────────
bool          nfcCardPresent = false;
unsigned long nfcDisplayTime = 0;
String        nfcData        = "";
String        nfcUID         = "";

// ══════════════════════════════════════════════════════════════════════════════
// nfcInit
// ══════════════════════════════════════════════════════════════════════════════
bool nfcInit() {
    nfc.begin();

    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        Serial.println("[NFC] PN532 not found – check wiring");
        Serial.println("[NFC] Expected: SCK=12, MISO=13, MOSI=11, SS=10");
        return false;
    }

    Serial.printf("[NFC] PN532 firmware v%d.%d found\n",
                  (ver >> 16) & 0xFF,
                  (ver >>  8) & 0xFF);
    nfc.SAMConfig();
    Serial.println("[NFC] Ready");
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Internal: Parse NTAG / MIFARE Ultralight (7-byte UID)
// ══════════════════════════════════════════════════════════════════════════════
static String parseNTAG(Adafruit_PN532& reader) {
    const int START_PAGE = 4;
    const int END_PAGE   = 39;

    uint8_t raw[4 * (END_PAGE - START_PAGE + 1)];
    int     rawLen = 0;

    for (int pg = START_PAGE; pg <= END_PAGE; pg++) {
        uint8_t pageData[4];
        if (!reader.mifareultralight_ReadPage(pg, pageData)) break;
        memcpy(raw + rawLen, pageData, 4);
        rawLen += 4;
    }

    String result = "";
    int pos = 0;
    while (pos < rawLen) {
        uint8_t tlvTag = raw[pos++];
        if (tlvTag == 0xFE) break;
        if (tlvTag == 0x00) continue;

        if (pos >= rawLen) break;
        int tlvLen = raw[pos++];
        if (tlvLen == 0xFF) {
            if (pos + 1 >= rawLen) break;
            tlvLen = ((int)raw[pos] << 8) | raw[pos + 1];
            pos += 2;
        }
        if (tlvTag != 0x03) { pos += tlvLen; continue; }

        if (pos >= rawLen) break;
        uint8_t recHeader = raw[pos++];
        bool    sr        = (recHeader & 0x10) != 0;
        bool    il        = (recHeader & 0x08) != 0;

        if (pos >= rawLen) break;
        uint8_t typeLen = raw[pos++];

        uint32_t payloadLen = 0;
        if (sr) {
            if (pos >= rawLen) break;
            payloadLen = raw[pos++];
        } else {
            if (pos + 3 >= rawLen) break;
            payloadLen = ((uint32_t)raw[pos]   << 24) |
                         ((uint32_t)raw[pos+1] << 16) |
                         ((uint32_t)raw[pos+2] <<  8) |
                          raw[pos+3];
            pos += 4;
        }

        uint8_t idLen = 0;
        if (il) { if (pos >= rawLen) break; idLen = raw[pos++]; }

        char recType[8] = {0};
        for (int t = 0; t < typeLen && t < 7 && pos < rawLen; t++)
            recType[t] = (char)raw[pos++];
        pos += idLen;

        if (strcmp(recType, "T") == 0) {
            if (pos >= rawLen || payloadLen < 1) break;
            uint8_t statusByte = raw[pos++];
            uint8_t langLen    = statusByte & 0x3F;
            pos += langLen;
            uint32_t textLen = payloadLen - 1 - langLen;
            for (uint32_t c = 0; c < textLen && pos < rawLen; c++)
                result += (char)raw[pos++];
        } else if (strcmp(recType, "U") == 0) {
            if (pos >= rawLen || payloadLen < 1) break;
            uint8_t prefixCode = raw[pos++];
            static const char* prefixes[] = {
                "", "http://www.", "https://www.", "http://", "https://",
                "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.",
                "ftps://", "sftp://", "smb://", "nfs://", "ftp://",
                "dav://", "news:", "telnet://", "imap:", "rtsp://",
                "urn:", "pop:", "sip:", "sips:", "tftp:", "btspp://",
                "btl2cap://", "btgoep://", "tcpobex://", "irdaobex://",
                "file://", "urn:epc:id:", "urn:epc:tag:", "urn:epc:pat:",
                "urn:epc:raw:", "urn:epc:", "urn:nfc:"
            };
            if (prefixCode < sizeof(prefixes) / sizeof(prefixes[0]))
                result += prefixes[prefixCode];
            uint32_t uriLen = payloadLen - 1;
            for (uint32_t c = 0; c < uriLen && pos < rawLen; c++)
                result += (char)raw[pos++];
        } else {
            for (uint32_t c = 0; c < payloadLen && pos < rawLen; c++) {
                uint8_t b = raw[pos++];
                if (b >= 32 && b <= 126) result += (char)b;
            }
        }
        break;
    }
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// Internal: Parse MIFARE Classic 1K (4-byte UID)
// ══════════════════════════════════════════════════════════════════════════════
static String parseMifareClassic(Adafruit_PN532& reader,
                                  uint8_t* uid, uint8_t uidLen) {
    uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    String  result  = "";

    if (!reader.mifareclassic_AuthenticateBlock(uid, uidLen, 4, 0, keyA))
        return result;

    uint8_t block[16];
    if (!reader.mifareclassic_ReadDataBlock(4, block))
        return result;

    int pos = 0;
    while (pos < 16 && block[pos] != 0x03) pos++;
    if (pos >= 16) return result;

    pos++; pos++; pos++; pos++; pos++; pos++;
    if (pos >= 16) return result;

    uint8_t langLen = block[pos++] & 0x3F;
    pos += langLen;

    while (pos < 16 && block[pos] != 0x00 && block[pos] != 0xFE) {
        if (block[pos] >= 32 && block[pos] <= 126)
            result += (char)block[pos];
        pos++;
    }
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// nfcProcessCard  (public)
// Build nfcUID, parse NDEF → nfcData.
// Feedback for scan-start is done here; success/error feedback is in main.cpp
// after the HTTP round-trip completes.
// ══════════════════════════════════════════════════════════════════════════════
void nfcProcessCard(uint8_t* uid, uint8_t uidLength) {
    // Build UID string
    nfcUID = "";
    for (uint8_t i = 0; i < uidLength; i++) {
        if (uid[i] < 0x10) nfcUID += "0";
        nfcUID += String(uid[i], HEX);
        if (i < uidLength - 1) nfcUID += ":";
    }
    nfcUID.toUpperCase();

    Serial.println("\n[NFC] Card detected — UID: " + nfcUID);

    // Parse NDEF
    if      (uidLength == 7) nfcData = parseNTAG(nfc);
    else if (uidLength == 4) nfcData = parseMifareClassic(nfc, uid, uidLength);
    else                     nfcData = "";

    nfcData.trim();

    if (nfcData.length() == 0) {
        Serial.println("[NFC] No NDEF text found");
    } else {
        Serial.println("[NFC] NDEF payload: " + nfcData);
    }
}