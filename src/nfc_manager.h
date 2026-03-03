//current project

#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ══════════════════════════════════════════════════════════════════════════════
// NFC MANAGER - Enhanced with Attendance Tracking
// Handles PN532 initialization, card detection, and NDEF payload parsing.
//
// Supports:
//   • 7-byte UID → NTAG / MIFARE Ultralight  (NDEF Text "T" + URI "U" records)
//   • 4-byte UID → MIFARE Classic 1K          (block-4 NDEF Text record)
// ══════════════════════════════════════════════════════════════════════════════

// ── Pin configuration (Software SPI) ─────────────────────────────────────────
#define PN532_SCK   12
#define PN532_MISO  13
#define PN532_MOSI  11
#define PN532_SS    10

// ── Public PN532 instance ─────────────────────────────────────────────────────
// Declared extern here so main.cpp can call readPassiveTargetID directly.
// Defined once in nfc_manager.cpp.
extern Adafruit_PN532 nfc;

// ── Public state (read by main / dashboard) ───────────────────────────────────
extern bool          nfcCardPresent;
extern unsigned long nfcDisplayTime;
extern String        nfcData;          // parsed NDEF payload (employee name if any)
extern String        nfcUID;           // colon-separated HEX UID string

// ── API ───────────────────────────────────────────────────────────────────────

/**
 * @brief  Initialise the PN532 module (Software SPI).
 *         Returns true on success; false if the chip is not found.
 */
bool nfcInit();

/**
 * @brief  Build the colon-separated HEX UID string and parse NDEF payload.
 *         Stores results in nfcUID and nfcData.
 *         Does NOT make HTTP calls — that is handled in main.cpp.
 * @param  uid        Raw UID bytes from readPassiveTargetID.
 * @param  uidLength  Number of valid UID bytes (4 or 7).
 */
void nfcProcessCard(uint8_t* uid, uint8_t uidLength);