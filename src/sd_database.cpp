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
#include "sd_mutex.h"

SemaphoreHandle_t g_sdMutex = nullptr;

bool SDDatabase::_ready = false;
DateProviderFn SDDatabase::_dateProvider = nullptr;
int SDDatabase::_cachedIns  = -1;
int SDDatabase::_cachedOuts = -1;

void SDDatabase::setDateProvider(DateProviderFn fn) {
    _dateProvider = fn;
    Serial.println("[SD] Date provider registered");
}

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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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
    ensureDir("/tap_log");   // raw debug log, separate from /attendance/

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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!SD_MMC.exists(path)) {
        if (!SD_MMC.mkdir(path)) {
            Serial.printf("[SD] Failed to create dir: %s\n", path);
            return false;
        }
        Serial.printf("[SD] Created dir: %s\n", path);
    }
    return true;
}

// Returns "/attendance/YYYY-MM-DD.csv" when NTP is synced,
// falls back to "/attendance/day_XXXXXX.csv" (uptime-based) otherwise.
// The NTP-based name resets naturally at midnight because the date changes.
String SDDatabase::todayFilename() {
    if (_dateProvider) {
        String d = _dateProvider();  // "YYYY-MM-DD" or ""
        if (d.length() == 10) {
            return "/attendance/" + d + ".csv";
        }
    }
    // Fallback: uptime-based (pre-NTP, device just booted)
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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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

    // Keep the in-memory today-count cache in sync incrementally instead of
    // invalidating it — this is what lets countTodayCheckIns()/
    // countTodayCheckOuts() stay O(1) after the first scan of the day. Only
    // bump the cache if it's already primed (_cachedIns >= 0); if this is
    // the very first write before anything has called countToday*() yet,
    // leave it at -1 so the next call does one real scan (which will count
    // this row correctly) rather than guessing at a partial state.
    if (eventType.endsWith("_in")) {
        if (_cachedIns >= 0) _cachedIns++;
    } else if (eventType.endsWith("_out")) {
        if (_cachedOuts >= 0) _cachedOuts++;
    }

    // Routed through SDLogger so it's automatically silenced during bulk
    // SERVER_SEED catch-up writes (which happen inside seedTodayAttendance-
    // FromServer()'s suspendSDWrite() window — see that function) while
    // staying fully visible for a real live NFC tap, which logs outside
    // that window. No extra condition needed: the suspend flag already
    // lines up with exactly the case we want quieted.
    SDLogger::logf("SD", SDLogger::INFO, "Logged: %s", row.c_str());
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// logRawTap — raw debug log, one row per physical NFC tap
// /tap_log/YYYY-MM-DD.csv (falls back to /tap_log/day_XXXXXX.csv pre-NTP,
// same pattern as todayFilename()). Deliberately dumb and separate from
// /attendance/: no event-type resolution, no employee-record lookups beyond
// the name already resolved by the caller, nothing here ever gets synced to
// the server or read back by the device — it's purely "what did the reader
// see and when", for tracing tap-to-record issues after the fact.
// ══════════════════════════════════════════════════════════════════════════════
String SDDatabase::todayTapLogFilename() {
    if (_dateProvider) {
        String d = _dateProvider();
        if (d.length() == 10) return "/tap_log/" + d + ".csv";
    }
    unsigned long day = millis() / 86400000UL;
    char buf[40];
    snprintf(buf, sizeof(buf), "/tap_log/day_%06lu.csv", day);
    return String(buf);
}

bool SDDatabase::logRawTap(const String& timeStr,
                            const String& employeeName,
                            const String& nfcUid) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready) return false;

    String fname = todayTapLogFilename();
    bool isNew = !SD_MMC.exists(fname);

    File f = SD_MMC.open(fname, FILE_APPEND);
    if (!f) {
        Serial.println("[SD] Cannot open tap log for append: " + fname);
        return false;
    }

    if (isNew) {
        f.println("time,name,nfc_uid");
    }

    String ts   = (timeStr.length() > 0) ? timeStr : String(millis());
    String name = (employeeName.length() > 0) ? employeeName : "UNKNOWN";
    String row  = csvEscape(ts) + "," + csvEscape(name) + "," + csvEscape(nfcUid);

    f.println(row);
    f.close();

    SDLogger::logf("SD", SDLogger::INFO, "TapLog: %s", row.c_str());
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// readTodayCSV / readCSV
// ══════════════════════════════════════════════════════════════════════════════
String SDDatabase::readTodayCSV() {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready) return "";
    return readCSV(todayFilename());
}

String SDDatabase::readCSV(const String& dateOrPath) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready) return "";
    String path = dateOrPath;
    if (!path.startsWith("/")) path = "/attendance/" + dateOrPath + ".csv";
    if (!SD_MMC.exists(path)) return "";
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return "";
    // Preallocate + chunked read instead of the old one-byte-at-a-time
    // `out += (char)f.read()` loop. String::concat() on this core reallocates
    // to the exact new size on every call rather than doubling, so appending
    // one char at a time on a multi-KB file meant a fresh heap realloc per
    // byte — expensive and fragmenting on a device that stays up for weeks.
    String out;
    out.reserve(f.size() + 1);
    uint8_t buf[512];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        out.concat((const char*)buf, n);
    }
    f.close();
    return out;
}

// ══════════════════════════════════════════════════════════════════════════════
// listAttendanceDates
// ══════════════════════════════════════════════════════════════════════════════
String SDDatabase::listAttendanceDates() {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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

// ── Count helpers: match all session variants ─────────────────────────────────
// Clock types written to CSV:  morning_in, morning_out, afternoon_in,
//                              afternoon_out, evening_in, evening_out
// A "check-in"  = any event ending in "_in"
// A "check-out" = any event ending in "_out"
//
// PERF: these used to do a full linear read of today's CSV on every single
// call (measured 50-200ms — see the STATE_NFC_PROFILE comment in main.cpp
// that used to defer this to the next loop() tick to avoid blocking NFC
// polling). Now cached in _cachedIns/_cachedOuts: the first call each day
// does one real scan to prime the cache, logAttendance() keeps it in sync
// incrementally on every write, and resetTodayCountCache() is called on
// midnight rollover so a new day starts from a fresh scan of the new file.
static int _scanEventSuffix(const String& path, const char* suffix) {
    if (!SD_MMC.exists(path)) return 0;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return 0;
    int count = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("timestamp")) continue;
        int c0 = line.indexOf(',');
        int c1 = (c0>=0) ? line.indexOf(',', c0+1) : -1;
        int c2 = (c1>=0) ? line.indexOf(',', c1+1) : -1;
        int c3 = (c2>=0) ? line.indexOf(',', c2+1) : -1;
        int c4 = (c3>=0) ? line.indexOf(',', c3+1) : -1;
        int c5 = (c4>=0) ? line.indexOf(',', c4+1) : -1;
        if (c4 < 0 || c5 < 0) continue;
        String ev = line.substring(c4+1, c5);
        ev.trim();
        if (ev.startsWith("\"")) ev = ev.substring(1);
        if (ev.endsWith("\""))   ev = ev.substring(0, ev.length()-1);
        if (ev.endsWith(suffix)) count++;
    }
    f.close();
    return count;
}

void SDDatabase::resetTodayCountCache() {
    _cachedIns  = -1;
    _cachedOuts = -1;
}

int SDDatabase::countTodayCheckIns() {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready) return 0;
    if (_cachedIns < 0) {
        _cachedIns = _scanEventSuffix(todayFilename(), "_in");
        Serial.printf("[SD] countTodayCheckIns = %d (full scan — cache primed)\n", _cachedIns);
    }
    return _cachedIns;
}

int SDDatabase::countTodayCheckOuts() {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready) return 0;
    if (_cachedOuts < 0) {
        _cachedOuts = _scanEventSuffix(todayFilename(), "_out");
        Serial.printf("[SD] countTodayCheckOuts = %d (full scan — cache primed)\n", _cachedOuts);
    }
    return _cachedOuts;
}

// ══════════════════════════════════════════════════════════════════════════════
// saveEmployeeProfile / loadEmployeeProfile / hasEmployeeProfile
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::saveEmployeeProfile(const String& empUid, const EmployeeProfile& emp) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || empUid.length() == 0) return false;
    return SD_MMC.exists("/employees/" + empUid + ".json");
}

// ══════════════════════════════════════════════════════════════════════════════
// savePhoto / hasPhoto / photoPath
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::savePhoto(const String& empUid, const uint8_t* data, size_t length) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || empUid.length() == 0 || !data || length == 0) return false;
    ensureDir("/photos");

    // Detect format from magic bytes to use correct extension
    String ext = ".jpg";
    if (length >= 12 &&
        data[0] == 0x52 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x46 &&
        data[8] == 0x57 && data[9] == 0x45 && data[10] == 0x42 && data[11] == 0x50) {
        ext = ".webp";
    } else if (length >= 4 &&
               data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        ext = ".png";
    }

    // Remove any existing photo (any format) before writing new one
    const char* exts[] = {".jpg", ".webp", ".png", nullptr};
    for (int i = 0; exts[i]; i++) {
        String old = "/photos/" + empUid + exts[i];
        if (SD_MMC.exists(old)) SD_MMC.remove(old);
    }

    String path = "/photos/" + empUid + ext;
    if (SD_MMC.exists(path)) SD_MMC.remove(path);

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] Cannot open for write: " + path);
        return false;
    }

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
        Serial.printf("[SD] Photo write incomplete: %u / %u\n", (unsigned)written, (unsigned)length);
        SD_MMC.remove(path);
        return false;
    }

    File vf = SD_MMC.open(path, FILE_READ);
    size_t onDisk = vf ? vf.size() : 0;
    if (vf) vf.close();
    if (onDisk != length) {
        Serial.printf("[SD] Size mismatch on disk %u vs expected %u\n", (unsigned)onDisk, (unsigned)length);
        SD_MMC.remove(path);
        return false;
    }

    Serial.printf("[SD] Saved photo: %s (%u bytes verified)\n", path.c_str(), (unsigned)length);
    return true;
}

bool SDDatabase::hasPhoto(const String& empUid) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || empUid.length() == 0) return false;
    // Check all supported formats — server may send WebP, JPEG, or PNG
    const char* exts[] = {".webp", ".jpg", ".png", nullptr};
    for (int i = 0; exts[i]; i++) {
        if (SD_MMC.exists("/photos/" + empUid + exts[i])) return true;
    }
    return false;
}

String SDDatabase::photoPath(const String& empUid) {
    // Return the path of whichever format is actually cached
    const char* exts[] = {".webp", ".jpg", ".png", nullptr};
    for (int i = 0; exts[i]; i++) {
        String p = "/photos/" + empUid + exts[i];
        if (SD_MMC.exists(p)) return p;
    }
    // Default fallback (file doesn't exist yet)
    return "/photos/" + empUid + ".jpg";
}

// ══════════════════════════════════════════════════════════════════════════════
// freeBytes / printInfo
// ══════════════════════════════════════════════════════════════════════════════
uint64_t SDDatabase::freeBytes() {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready) return 0;
    return SD_MMC.totalBytes() - SD_MMC.usedBytes();
}

void SDDatabase::printInfo() {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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
// loadAttendanceToday
// Returns comma-separated clock_types logged today for a specific empUid.
// CSV cols: timestamp,nfc_uid,employee_uid,name,dept,event_type,device_id
//                       col2                          col5
// e.g. "morning_in,morning_out,afternoon_in"
// ══════════════════════════════════════════════════════════════════════════════
String SDDatabase::loadAttendanceToday(const String& empUid) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready) return "";
    String path = todayFilename();
    if (!SD_MMC.exists(path)) return "";
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return "";

    String result = "";
    bool   first  = true;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        if (line.startsWith("timestamp")) continue;  // skip header row

        // Parse CSV: col0=timestamp, col1=nfc_uid, col2=employee_uid,
        //            col3=name, col4=department, col5=event_type, col6=device_id
        int c0 = line.indexOf(',');
        int c1 = (c0>=0) ? line.indexOf(',', c0+1) : -1;
        int c2 = (c1>=0) ? line.indexOf(',', c1+1) : -1;
        int c3 = (c2>=0) ? line.indexOf(',', c2+1) : -1;
        int c4 = (c3>=0) ? line.indexOf(',', c3+1) : -1;
        int c5 = (c4>=0) ? line.indexOf(',', c4+1) : -1;
        if (c1<0 || c2<0 || c4<0 || c5<0) continue;

        // col2 = employee_uid
        String uid = line.substring(c1+1, c2);
        uid.trim();
        // strip surrounding quotes if csvEscape added them
        if (uid.startsWith("\"")) uid = uid.substring(1);
        if (uid.endsWith("\""))   uid = uid.substring(0, uid.length()-1);

        if (uid != empUid) continue;

        // col5 = event_type (clock_type)
        String evType = line.substring(c4+1, c5);
        evType.trim();
        if (evType.startsWith("\"")) evType = evType.substring(1);
        if (evType.endsWith("\""))   evType = evType.substring(0, evType.length()-1);

        if (evType.length() == 0) continue;

        if (!first) result += ",";
        result += evType;
        first = false;
    }
    f.close();

    // Routed through SDLogger (not a raw Serial.println) so this respects
    // suspendSDWrite() during a seed/reconcile pass. This is the single
    // highest-volume trace line in the whole codebase — called once per
    // employee per pass, 3 passes per cycle, so ~140+ times per seed cycle
    // against a ~48-employee roster. Left as a raw Serial call, it was the
    // main remaining source of garbled/interleaved terminal output during a
    // seed pass even after suspendSDWrite() was made Serial-aware, since it
    // never went through the logger to begin with.
    SDLogger::logf("SD", SDLogger::INFO, "loadAttendanceToday uid=%s -> '%s'",
                    empUid.c_str(), result.c_str());
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// removeAttendanceRow
//
// Rewrites TODAY's CSV, dropping every row whose (employee_uid, event_type)
// matches the given pair. FAT32 / SD_MMC has no "delete this one line" API,
// so this reads the whole file into a String, filters, then reopens with
// FILE_WRITE (which truncates) and writes the filtered content back.
//
// Today's CSV is small (tens to low-hundreds of rows), so buffering the
// whole thing in RAM is fine — this mirrors the same read-all pattern
// already used by loadAttendanceToday()/countTodayCheckIns().
// ══════════════════════════════════════════════════════════════════════════════
bool SDDatabase::removeAttendanceRow(const String& empUid, const String& clockType) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || empUid.length() == 0 || clockType.length() == 0) return false;

    String path = todayFilename();
    if (!SD_MMC.exists(path)) return true;  // nothing to remove — already "gone"

    File rf = SD_MMC.open(path, FILE_READ);
    if (!rf) {
        Serial.println("[SD] removeAttendanceRow: cannot open for read: " + path);
        return false;
    }

    String keptLines;
    bool   removedAny   = false;
    int    removedInCt  = 0;
    int    removedOutCt = 0;

    while (rf.available()) {
        String line = rf.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        if (line.startsWith("timestamp")) {
            keptLines += line + "\n";
            continue;
        }

        // CSV: timestamp,nfc_uid,employee_uid,name,dept,event_type,device_id
        int c0 = line.indexOf(',');
        int c1 = (c0>=0) ? line.indexOf(',', c0+1) : -1;
        int c2 = (c1>=0) ? line.indexOf(',', c1+1) : -1;
        int c3 = (c2>=0) ? line.indexOf(',', c2+1) : -1;
        int c4 = (c3>=0) ? line.indexOf(',', c3+1) : -1;
        int c5 = (c4>=0) ? line.indexOf(',', c4+1) : -1;

        bool matches = false;
        if (c1 >= 0 && c2 >= 0 && c4 >= 0 && c5 >= 0) {
            String uid = line.substring(c1+1, c2);
            uid.trim();
            if (uid.startsWith("\"")) uid = uid.substring(1);
            if (uid.endsWith("\""))   uid = uid.substring(0, uid.length()-1);

            String evType = line.substring(c4+1, c5);
            evType.trim();
            if (evType.startsWith("\"")) evType = evType.substring(1);
            if (evType.endsWith("\""))   evType = evType.substring(0, evType.length()-1);

            if (uid == empUid && evType == clockType) matches = true;
        }

        if (matches) {
            removedAny = true;
            if (clockType.endsWith("_in"))  removedInCt++;
            if (clockType.endsWith("_out")) removedOutCt++;
            Serial.println("[SD] removeAttendanceRow: dropping -> " + line);
        } else {
            keptLines += line + "\n";
        }
        yield();
    }
    rf.close();

    if (!removedAny) {
        // Nothing matched — file already reflects the desired state.
        return true;
    }

    File wf = SD_MMC.open(path, FILE_WRITE);  // FILE_WRITE truncates on ESP32
    if (!wf) {
        Serial.println("[SD] removeAttendanceRow: cannot reopen for write: " + path);
        return false;
    }
    wf.print(keptLines);
    wf.close();

    // Keep the count cache in sync with what was actually dropped, same
    // reasoning as the increment in logAttendance(). Only adjust if primed
    // (>= 0); clamp at 0 defensively so a mismatched cache can never go
    // negative and poison future increments.
    if (_cachedIns  >= 0) _cachedIns  = max(0, _cachedIns  - removedInCt);
    if (_cachedOuts >= 0) _cachedOuts = max(0, _cachedOuts - removedOutCt);

    Serial.println("[SD] removeAttendanceRow: uid=" + empUid +
                   " type=" + clockType + " removed — CSV rewritten (" + path + ")");
    return true;
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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
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

// ══════════════════════════════════════════════════════════════════════════════
// loadUidForNfcPrefix
//
// Fallback for a short/truncated NDEF read (card left the RF field before the
// full ID was captured — see nfc_manager.cpp). Scans /employees for mapping
// files whose name starts with "nfc_<prefix>". Only returns a uid when
// EXACTLY ONE file matches — 0 matches (unknown) or 2+ matches (ambiguous
// prefix shared by more than one employee) both return "" so callers never
// silently clock in the wrong person. Caller is responsible for only
// invoking this with a prefix long enough to be safe (see MIN_PREFIX_LEN in
// main.cpp) — this function itself has no opinion on minimum length.
// ══════════════════════════════════════════════════════════════════════════════
String SDDatabase::loadUidForNfcPrefix(const String& prefix) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || prefix.length() == 0) return "";

    File dir = SD_MMC.open("/employees");
    if (!dir || !dir.isDirectory()) return "";

    String safePrefix = _sanitizeForFilename(prefix);
    String needle      = "nfc_" + safePrefix;

    String matchUid   = "";
    int    matchCount = 0;

    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String name = String(f.name());
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);

            if (name.startsWith(needle)) {
                matchCount++;
                if (matchCount == 1) {
                    DynamicJsonDocument doc(128);
                    if (deserializeJson(doc, f) == DeserializationError::Ok) {
                        matchUid = doc["uid"] | "";
                    }
                } else {
                    // Second+ match found — no need to keep scanning further,
                    // the prefix is already proven ambiguous.
                    f.close();
                    break;
                }
            }
        }
        f = dir.openNextFile();
    }
    dir.close();

    if (matchCount != 1 || matchUid.length() == 0) {
        Serial.printf("[SD] loadUidForNfcPrefix: prefix '%s' matched %d employee(s) — ambiguous/unknown, rejecting\n",
                      prefix.c_str(), matchCount);
        return "";
    }

    Serial.printf("[SD] loadUidForNfcPrefix: prefix '%s' uniquely matched empUid=%s\n",
                  prefix.c_str(), matchUid.c_str());
    return matchUid;
}

// ══════════════════════════════════════════════════════════════════════════════
// Server-ID Map  — /attendance/server_ids_YYYY-MM-DD.json
// Stores the server DB record `id` returned after a successful attendance POST
// so edits can use PUT /{id} instead of POST, preventing duplicate rows.
// Key: "empUid|clockType|HH:MM:SS"
//
// PERF: this used to be a single JSON object, read-parsed-modified-
// reserialized-rewritten on EVERY save (full O(n) read+write per tap, plus a
// fixed 4096-byte DynamicJsonDocument capacity that silently stops accepting
// new keys once ~80-100 entries are in it — no error, just quietly-dropped
// mappings past that point, which then fall back to POST and risk duplicate
// server rows). Now stored as flat append-only "key=id" lines on disk:
// saves are O(1) appends with no capacity ceiling, and the O(n) cost only
// happens on the rarer read paths (portal viewing the map, or an edit/
// delete). loadServerIdMapJson() still hands back a proper JSON object so
// existing consumers (WiFiManager.h, attendance_http_service.h) don't need
// to change at all.
// ══════════════════════════════════════════════════════════════════════════════

static String _serverIdMapPath(const String& date) {
    return "/attendance/server_ids_" + date + ".json";
}

// One-time migration: if a file from before this update still holds the old
// single-JSON-object format (starts with '{'), convert it to flat "key=id"
// lines in place. No-op (fast peek + close) once a file is already flat, so
// this is cheap to call unconditionally at the top of every map operation —
// it only does real work the first time this firmware touches a date file
// left over from the previous build.
static void _migrateServerIdMapIfNeeded(const String& path) {
    if (!SD_MMC.exists(path)) return;
    File pf = SD_MMC.open(path, FILE_READ);
    if (!pf) return;
    int firstByte = pf.peek();
    if (firstByte != '{') { pf.close(); return; }   // already flat, or empty

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, pf);
    pf.close();
    if (err) {
        Serial.println("[SD] server-id map migration: parse failed, leaving file as-is: " + path);
        return;   // don't risk data loss on a parse error — leave the old file alone
    }

    File wf = SD_MMC.open(path, FILE_WRITE);   // truncates
    if (!wf) return;
    for (JsonPair kv : doc.as<JsonObject>()) {
        wf.print(kv.key().c_str());
        wf.print('=');
        wf.println((long)kv.value().as<long>());
    }
    wf.close();
    Serial.println("[SD] Migrated server-id map to flat format: " + path);
}

static String _serverIdKey(const String& empUid, const String& clockType, const String& timeStr) {
    String t = timeStr;
    int sp = t.indexOf(' ');
    if (sp >= 0) t = t.substring(sp + 1);   // "YYYY-MM-DD HH:MM:SS" → "HH:MM:SS"
    return empUid + "|" + clockType + "|" + t;
}

bool SDDatabase::saveServerIdMapping(const String& date, const String& empUid,
                                      const String& clockType, const String& timeStr,
                                      int serverId) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || serverId <= 0 || date.length() == 0) return false;

    String path = _serverIdMapPath(date);
    _migrateServerIdMapIfNeeded(path);

    String key = _serverIdKey(empUid, clockType, timeStr);

    File wf = SD_MMC.open(path, FILE_APPEND);
    if (!wf) {
        Serial.println("[SD] saveServerIdMapping: cannot open " + path);
        return false;
    }
    wf.print(key);
    wf.print('=');
    wf.println(serverId);
    wf.close();

    Serial.printf("[SD] Saved server_id=%d for key=%s\n", serverId, key.c_str());
    return true;
}

int SDDatabase::getServerIdForRecord(const String& date, const String& empUid,
                                      const String& clockType, const String& timeStr) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || date.length() == 0) return 0;

    String path = _serverIdMapPath(date);
    if (!SD_MMC.exists(path)) return 0;
    _migrateServerIdMapIfNeeded(path);

    String needle = _serverIdKey(empUid, clockType, timeStr) + "=";

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return 0;
    int result = 0;
    // Scan to the end rather than stopping at the first match: if the same
    // key was ever saved twice (re-sync, retry), the LAST line wins, same
    // "last write" semantics the old doc[key]=val approach had.
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        if (line.startsWith(needle)) {
            result = line.substring(needle.length()).toInt();
        }
    }
    f.close();
    return result;
}

String SDDatabase::loadServerIdMapJson(const String& date) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || date.length() == 0) return "{}";
    String path = _serverIdMapPath(date);
    if (!SD_MMC.exists(path)) return "{}";
    _migrateServerIdMapIfNeeded(path);

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return "{}";
    DynamicJsonDocument doc(8192);
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        int eq = line.lastIndexOf('=');
        if (eq < 0) continue;
        doc[line.substring(0, eq)] = line.substring(eq + 1).toInt();
        yield();
    }
    f.close();
    String out;
    serializeJson(doc, out);
    return out;
}

bool SDDatabase::removeServerIdMapping(const String& date, const String& empUid,
                                        const String& clockType, const String& timeStr) {
    SDLockGuard _sdLock;   // serialize SD_MMC access across tasks (see sd_mutex.h)
    if (!_ready || date.length() == 0) return false;

    String path = _serverIdMapPath(date);
    if (!SD_MMC.exists(path)) return true;   // nothing to remove
    _migrateServerIdMapIfNeeded(path);

    String needle = _serverIdKey(empUid, clockType, timeStr) + "=";

    File rf = SD_MMC.open(path, FILE_READ);
    if (!rf) return false;
    String kept;
    bool removedAny = false;
    while (rf.available()) {
        String line = rf.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        if (line.startsWith(needle)) {
            removedAny = true;
        } else {
            kept += line + "\n";
        }
        yield();
    }
    rf.close();

    if (!removedAny) return true;   // key not found — already gone

    File wf = SD_MMC.open(path, FILE_WRITE);   // truncates
    if (!wf) {
        Serial.println("[SD] removeServerIdMapping: cannot reopen for write: " + path);
        return false;
    }
    wf.print(kept);
    wf.close();
    return true;
}