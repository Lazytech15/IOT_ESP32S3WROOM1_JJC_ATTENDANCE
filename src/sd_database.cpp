// ══════════════════════════════════════════════════════════════════════════════
// sd_database.cpp  — uses SD_MMC (SDMMC 1-bit, pins 38/39/40)
//
// FIX v2 — NFC UID filenames now use sanitized names.
//
// ROOT CAUSE OF MISSING EMPLOYEE PROFILE DISPLAY:
//   NFC card UIDs look like "04:A3:2F:12:6B:4C:80".
//   saveNfcMapping() was constructing filenames like:
//       /employees/nfc_04:A3:2F:12:6B:4C:80.json
//   The SD card is formatted as FAT32, which does NOT allow colons ':' in
//   filenames. SD_MMC.open() silently fails to create such a file, so the
//   mapping was never written.
//   loadUidForNfc() therefore always returned "" for every card scan,
//   the employee lookup fell through to the server every time, and the
//   photo path was always empty on the first scan → only initials drawn.
//
// FIX:
//   _sanitizeForFilename() replaces every ':' with '-' before the filename
//   is composed. Files are now stored as e.g.:
//       /employees/nfc_04-A3-2F-12-6B-4C-80.json
//   Both saveNfcMapping() and loadUidForNfc() use the same helper so
//   they always agree on the filename.
// ══════════════════════════════════════════════════════════════════════════════

#include "sd_database.h"
#include "sd_logger.h"

bool SDDatabase::_ready = false;

// ── Private helper: replace characters illegal on FAT32 ──────────────────────
// FAT32 forbids: \ / : * ? " < > |
// NFC UIDs only ever contain hex digits and ':', so we just replace ':' → '-'.
static String _sanitizeForFilename(const String& s) {
    String out = s;
    out.replace(":", "-");   // "04:A3:2F" → "04-A3-2F"
    out.replace("/", "-");
    out.replace("\\", "-");
    out.replace("*", "-");
    out.replace("?", "-");
    out.replace("\"", "-");
    out.replace("<", "-");
    out.replace(">", "-");
    out.replace("|", "-");
    return out;
}

// ══════════════════════════════════════════════════════════════════════════════
// begin
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::begin() {
    Serial.println("[SD] Initializing SD card (SDMMC 1-bit)...");
    Serial.printf("[SD] Pins — CLK:%d  CMD:%d  D0:%d\n",
                  SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);

    // Set custom pins BEFORE begin()
    SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);

    // begin(mountpoint, mode1bit=true)
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("[SD] ERROR: Mount failed — check card inserted & FAT32 formatted");
        _ready = false;
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD] ERROR: No card detected");
        SD_MMC.end();
        _ready = false;
        return false;
    }

    const char* typeName = "UNKNOWN";
    switch (cardType) {
        case CARD_MMC:  typeName = "MMC";  break;
        case CARD_SD:   typeName = "SD";   break;
        case CARD_SDHC: typeName = "SDHC"; break;
    }

    uint64_t totalMB = SD_MMC.totalBytes() / (1024 * 1024);
    uint64_t usedMB  = SD_MMC.usedBytes()  / (1024 * 1024);
    Serial.printf("[SD] %s  |  %llu MB total  |  %llu MB used\n",
                  typeName, totalMB, usedMB);

    // Create required directories
    ensureDir("/attendance");
    ensureDir("/employees");
    ensureDir("/photos");

    _ready = true;
    ensureDir("/logs");
    SDLogger::begin();
    Serial.println("[SD] Ready");
    return true;
}

bool SDDatabase::isReady() { return _ready; }

// ══════════════════════════════════════════════════════════════════════════════
// Helpers
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::ensureDir(const char* path) {
    if (!SD_MMC.exists(path)) {
        if (!SD_MMC.mkdir(path)) {
            Serial.printf("[SD] Failed to create dir: %s\n", path);
            return false;
        }
        Serial.printf("[SD] Created dir: %s\n", path);
    }
    return true;
}

// "/attendance/day_XXXXXX.csv" — increments per 24h of uptime
// Replace with NTP-based date if you have NTP set up.
String SDDatabase::todayFilename() {
    unsigned long day = millis() / 86400000UL;
    char buf[40];
    snprintf(buf, sizeof(buf), "/attendance/day_%06lu.csv", day);
    return String(buf);
}

String SDDatabase::csvEscape(const String& s) {
    if (s.indexOf(',') >= 0 || s.indexOf('"') >= 0 || s.indexOf('\n') >= 0) {
        String e = "\"";
        for (char c : s) {
            if (c == '"') e += "\"\"";
            else          e += c;
        }
        e += "\"";
        return e;
    }
    return s;
}

// ══════════════════════════════════════════════════════════════════════════════
// logAttendance
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::logAttendance(const String& timestamp,
                                const String& nfcUid,
                                const EmployeeProfile& emp,
                                const String& eventType,
                                const String& deviceId) {
    if (!_ready) return false;

    String fname = todayFilename();
    bool isNew = !SD_MMC.exists(fname);

    File f = SD_MMC.open(fname, FILE_APPEND);
    if (!f) {
        Serial.println("[SD] Cannot open for append: " + fname);
        return false;
    }

    if (isNew) {
        f.println("timestamp,nfc_uid,employee_uid,employee_name,department,event_type,device_id");
    }

    String ts  = (timestamp.length() > 0) ? timestamp : String(millis());
    String row = csvEscape(ts)              + "," +
                 csvEscape(nfcUid)          + "," +
                 csvEscape(emp.uid)         + "," +
                 csvEscape(emp.fullName)    + "," +
                 csvEscape(emp.department)  + "," +
                 csvEscape(eventType)       + "," +
                 csvEscape(deviceId);

    f.println(row);
    f.close();

    Serial.println("[SD] Logged: " + row);
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// readTodayCSV / readCSV
// ══════════════════════════════════════════════════════════════════════════════
String SDDatabase::readTodayCSV() {
    if (!_ready) return "";
    return readCSV(todayFilename());
}

String SDDatabase::readCSV(const String& dateOrPath) {
    if (!_ready) return "";
    String path = dateOrPath;
    if (!path.startsWith("/")) path = "/attendance/" + dateOrPath + ".csv";
    if (!SD_MMC.exists(path)) return "";
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return "";
    String out;
    while (f.available()) out += (char)f.read();
    f.close();
    return out;
}

// ══════════════════════════════════════════════════════════════════════════════
// listAttendanceDates
// ══════════════════════════════════════════════════════════════════════════════
String SDDatabase::listAttendanceDates() {
    if (!_ready) return "";
    File dir = SD_MMC.open("/attendance");
    if (!dir) return "";
    String out;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) out += String(entry.name()) + "\n";
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return out;
}

// ══════════════════════════════════════════════════════════════════════════════
// countTodayCheckIns / countTodayCheckOuts
// ══════════════════════════════════════════════════════════════════════════════
int SDDatabase::countEventInCSV(const String& path, const String& eventType) {
    if (!_ready) return -1;
    if (!SD_MMC.exists(path)) return 0;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return -1;
    int count = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.indexOf(eventType) >= 0) count++;
    }
    f.close();
    return count;
}

int SDDatabase::countTodayCheckIns()  { return countEventInCSV(todayFilename(), "check-in");  }
int SDDatabase::countTodayCheckOuts() { return countEventInCSV(todayFilename(), "check-out"); }

// ══════════════════════════════════════════════════════════════════════════════
// saveEmployeeProfile / loadEmployeeProfile / hasEmployeeProfile
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::saveEmployeeProfile(const String& empUid, const EmployeeProfile& emp) {
    if (!_ready || empUid.length() == 0) return false;

    String path = "/employees/" + empUid + ".json";
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) { Serial.println("[SD] Cannot write employee: " + path); return false; }

    DynamicJsonDocument doc(1024);
    doc["uid"]            = emp.uid;
    doc["idNumber"]       = emp.idNumber;
    doc["fullName"]       = emp.fullName;
    doc["firstName"]      = emp.firstName;
    doc["lastName"]       = emp.lastName;
    doc["position"]       = emp.position;
    doc["department"]     = emp.department;
    doc["email"]          = emp.email;
    doc["status"]         = emp.status;
    doc["employmentType"] = emp.employmentType;
    doc["accessGranted"]  = emp.accessGranted;
    doc["profilePicture"] = emp.profilePicture;

    serializeJson(doc, f);
    f.close();
    Serial.println("[SD] Saved employee: " + path);
    return true;
}

bool SDDatabase::loadEmployeeProfile(const String& empUid, EmployeeProfile& out) {
    if (!_ready || empUid.length() == 0) return false;

    String path = "/employees/" + empUid + ".json";
    if (!SD_MMC.exists(path)) return false;

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.println("[SD] loadEmployee parse error: " + String(err.c_str()));
        return false;
    }

    out.uid            = doc["uid"]            | "";
    out.idNumber       = doc["idNumber"]       | "";
    out.fullName       = doc["fullName"]       | "";
    out.firstName      = doc["firstName"]      | "";
    out.lastName       = doc["lastName"]       | "";
    out.position       = doc["position"]       | "";
    out.department     = doc["department"]     | "";
    out.email          = doc["email"]          | "";
    out.status         = doc["status"]         | "Active";
    out.employmentType = doc["employmentType"] | "";
    out.accessGranted  = doc["accessGranted"]  | false;
    out.profilePicture = doc["profilePicture"] | "";
    out.hasData        = true;

    Serial.println("[SD] Loaded cached: " + out.fullName + "  uid=" + out.uid +
                   "  photo=" + out.profilePicture);
    return true;
}

bool SDDatabase::hasEmployeeProfile(const String& empUid) {
    if (!_ready || empUid.length() == 0) return false;
    return SD_MMC.exists("/employees/" + empUid + ".json");
}

// ══════════════════════════════════════════════════════════════════════════════
// savePhoto / hasPhoto / photoPath
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::savePhoto(const String& empUid, const uint8_t* data, size_t length) {
    if (!_ready || empUid.length() == 0 || !data || length == 0) {
        Serial.printf("[SD] savePhoto SKIP: ready=%d uid=%s len=%u\n",
                      _ready, empUid.c_str(), (unsigned)length);
        return false;
    }

    ensureDir("/photos");

    String path = "/photos/" + empUid + ".jpg";

    // Remove any stale file before writing
    if (SD_MMC.exists(path)) SD_MMC.remove(path);

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] Cannot open for write: " + path);
        return false;
    }

    // Write in 4 KB chunks with yield() to avoid WDT resets
    const size_t CHUNK = 4096;
    size_t written = 0;
    while (written < length) {
        size_t toWrite = min(CHUNK, length - written);
        size_t w = f.write(data + written, toWrite);
        written += w;
        if (w != toWrite) break;
        yield();
    }

    f.flush();
    f.close();

    if (written != length) {
        Serial.printf("[SD] Photo write incomplete: %u / %u\n",
                      (unsigned)written, (unsigned)length);
        SD_MMC.remove(path);
        return false;
    }

    // Verify the file actually landed on the card with correct size
    if (!SD_MMC.exists(path)) {
        Serial.println("[SD] Photo missing after write: " + path);
        return false;
    }
    File vf = SD_MMC.open(path, FILE_READ);
    size_t onDisk = vf ? vf.size() : 0;
    if (vf) vf.close();
    if (onDisk != length) {
        Serial.printf("[SD] Size mismatch on disk %u vs expected %u\n",
                      (unsigned)onDisk, (unsigned)length);
        SD_MMC.remove(path);
        return false;
    }

    Serial.printf("[SD] Saved photo: %s (%u bytes verified)\n",
                  path.c_str(), (unsigned)length);
    return true;
}

bool SDDatabase::hasPhoto(const String& empUid) {
    if (!_ready || empUid.length() == 0) return false;
    return SD_MMC.exists("/photos/" + empUid + ".jpg");
}

String SDDatabase::photoPath(const String& empUid) {
    return "/photos/" + empUid + ".jpg";
}

// ══════════════════════════════════════════════════════════════════════════════
// freeBytes / printInfo
// ══════════════════════════════════════════════════════════════════════════════
uint64_t SDDatabase::freeBytes() {
    if (!_ready) return 0;
    return SD_MMC.totalBytes() - SD_MMC.usedBytes();
}

void SDDatabase::printInfo() {
    if (!_ready) { Serial.println("[SD] Not mounted"); return; }
    Serial.printf("[SD] Total: %llu MB  Used: %llu MB  Free: %llu MB\n",
                  SD_MMC.totalBytes() / 1048576,
                  SD_MMC.usedBytes()  / 1048576,
                  freeBytes()         / 1048576);
    Serial.println("[SD] Attendance files:\n" + listAttendanceDates());
}

// ══════════════════════════════════════════════════════════════════════════════
// hasCheckedInToday
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::hasCheckedInToday(const String& empUid) {
    if (!_ready) return false;
    String path = todayFilename();
    if (!SD_MMC.exists(path)) return false;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    String lastEvent = "";
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        // CSV cols: timestamp,nfc_uid,employee_uid,name,dept,event_type,device_id
        int c0 = line.indexOf(',');
        int c1 = (c0>=0) ? line.indexOf(',',c0+1) : -1;
        int c2 = (c1>=0) ? line.indexOf(',',c1+1) : -1;
        int c3 = (c2>=0) ? line.indexOf(',',c2+1) : -1;
        int c4 = (c3>=0) ? line.indexOf(',',c3+1) : -1;
        int c5 = (c4>=0) ? line.indexOf(',',c4+1) : -1;
        if (c1<0||c2<0||c4<0||c5<0) continue;
        String uid = line.substring(c1+1,c2); uid.trim();
        if (uid == empUid) {
            lastEvent = line.substring(c4+1,c5); lastEvent.trim();
            if (lastEvent.startsWith("\"")) lastEvent=lastEvent.substring(1);
            if (lastEvent.endsWith("\""))   lastEvent=lastEvent.substring(0,lastEvent.length()-1);
        }
    }
    f.close();
    return (lastEvent == "check-in");
}

// ══════════════════════════════════════════════════════════════════════════════
// saveNfcMapping / loadUidForNfc
//
// FIX: NFC UIDs contain colons (e.g. "04:A3:2F:12:6B:4C:80").
// FAT32 forbids ':' in filenames — SD_MMC.open() silently fails on such names,
// so the mapping was NEVER written and NEVER read back.
//
// Solution: _sanitizeForFilename() replaces ':' with '-' before composing
// the path. Both functions use the same helper so they always agree.
//
// Old filename: /employees/nfc_04:A3:2F:12:6B:4C:80.json  ← INVALID on FAT32
// New filename: /employees/nfc_04-A3-2F-12-6B-4C-80.json  ← valid
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::saveNfcMapping(const String& cardId, const String& empUid) {
    if (!_ready || cardId.length() == 0 || empUid.length() == 0) return false;

    String safeId = _sanitizeForFilename(cardId);
    String path   = "/employees/nfc_" + safeId + ".json";

    Serial.println("[SD] saveNfcMapping: " + cardId + " → " + empUid +
                   "  file=" + path);

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] saveNfcMapping: open FAILED for " + path);
        return false;
    }
    f.print("{\"uid\":\""); f.print(empUid); f.print("\"}");
    f.close();

    Serial.println("[SD] saveNfcMapping: saved OK");
    return true;
}

String SDDatabase::loadUidForNfc(const String& cardId) {
    if (!_ready || cardId.length() == 0) return "";

    // First check: if cardId itself is an employee uid (numeric id scan)
    if (SD_MMC.exists("/employees/" + cardId + ".json")) return cardId;

    // Second check: NFC mapping file (use same sanitized filename)
    String safeId = _sanitizeForFilename(cardId);
    String mp = "/employees/nfc_" + safeId + ".json";

    Serial.println("[SD] loadUidForNfc: looking for " + mp);

    if (!SD_MMC.exists(mp)) {
        Serial.println("[SD] loadUidForNfc: not found → will go to server");
        return "";
    }

    File f = SD_MMC.open(mp, FILE_READ);
    if (!f) return "";

    DynamicJsonDocument doc(128);
    deserializeJson(doc, f);
    f.close();

    String uid = doc["uid"] | "";
    Serial.println("[SD] loadUidForNfc: found empUid=" + uid);
    return uid;
}