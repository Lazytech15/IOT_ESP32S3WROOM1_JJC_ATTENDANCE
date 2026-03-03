// ══════════════════════════════════════════════════════════════════════════════
// sd_logger.h  — Persistent SD Card Logger
//
// Writes timestamped log entries to /logs/system_<day>.log on the SD card.
// Provides a simple static API so any module can call SDLogger::log() without
// holding a file handle open (file is opened, appended, and closed each call
// to avoid handle leaks across the async event loop).
//
// Log format (plain text, one line per entry):
//   [DAY_HHMMSS][LEVEL][TAG] message
//   e.g. [000_081523][INFO][IMG] JPEG decode OK - 14320 bytes
//
// Levels: DEBUG, INFO, WARN, ERROR
//
// Usage:
//   SDLogger::log("IMG",  SDLogger::INFO,  "JPEG decode OK");
//   SDLogger::logf("IMG", SDLogger::ERROR, "TJpgDec error %d", res);
//   SDLogger::dumpToSerial(50);   // print last 50 lines to Serial
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <SD_MMC.h>

class SDLogger {
public:
    enum Level { DEBUG = 0, INFO, WARN, ERROR };

    // ── begin ─────────────────────────────────────────────────────────────────
    // Call once after SDDatabase::begin() succeeds.
    static void begin() {
        _ready = true;
        _ensureDir();
        log("LOG", INFO, "=== SD Logger started ===");
        Serial.println("[SDLogger] Logging to " + _logPath());
        Serial.flush();
    }

    // ── log ───────────────────────────────────────────────────────────────────
    static void log(const char* tag, Level level, const String& msg) {
        // Always echo to Serial
        String prefix = _prefix(tag, level);
        Serial.println(prefix + msg);
        Serial.flush();

        if (!_ready) return;

        File f = SD_MMC.open(_logPath(), FILE_APPEND);
        if (!f) return;
        f.println(prefix + msg);
        f.flush();
        f.close();
    }

    // ── logf (printf-style) ───────────────────────────────────────────────────
    static void logf(const char* tag, Level level, const char* fmt, ...) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log(tag, level, String(buf));
    }

    // ── dumpToSerial ──────────────────────────────────────────────────────────
    // Print the last `maxLines` lines of today's log to Serial.
    static void dumpToSerial(int maxLines = 100) {
        if (!_ready) { Serial.println("[SDLogger] Not ready"); return; }
        String path = _logPath();
        if (!SD_MMC.exists(path)) {
            Serial.println("[SDLogger] No log file: " + path);
            return;
        }
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) return;

        // Collect all lines (SD files are small — max ~64 KB per day)
        String lines[200];
        int count = 0;
        while (f.available() && count < 200) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) lines[count++ % 200] = line;
        }
        f.close();

        int start = (count > maxLines) ? (count - maxLines) : 0;
        Serial.println("─── SD Log: " + path + " (" + String(count) + " lines) ───");
        for (int i = start; i < count; i++)
            Serial.println(lines[i % 200]);
        Serial.println("─── End of log ───");
        Serial.flush();
    }

    // ── readLog ───────────────────────────────────────────────────────────────
    // Return full log file content as a String (for web portal).
    static String readLog(int dayOffset = 0) {
        if (!_ready) return "";
        String path = _logPath(dayOffset);
        if (!SD_MMC.exists(path)) return "(no log for this day)";
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) return "(open failed)";
        String out = "";
        while (f.available()) {
            out += (char)f.read();
            if (out.length() > 65536) { out += "\n...(truncated)"; break; }
        }
        f.close();
        return out;
    }

    // ── listLogFiles ─────────────────────────────────────────────────────────
    static String listLogFiles() {
        if (!_ready) return "";
        if (!SD_MMC.exists("/logs")) return "";
        File dir = SD_MMC.open("/logs");
        if (!dir) return "";
        String out = "";
        File entry = dir.openNextFile();
        while (entry) {
            if (!entry.isDirectory())
                out += String(entry.name()) + "  " + String(entry.size()) + " B\n";
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
        return out;
    }

    static bool isReady() { return _ready; }

private:
    inline static bool _ready = false;

    static void _ensureDir() {
        if (!SD_MMC.exists("/logs")) SD_MMC.mkdir("/logs");
    }

    // Log file name keyed by uptime day (same scheme as attendance CSV)
    static String _logPath(int dayOffset = 0) {
        unsigned long day = millis() / 86400000UL + dayOffset;
        char buf[40];
        snprintf(buf, sizeof(buf), "/logs/system_%06lu.log", day);
        return String(buf);
    }

    static String _prefix(const char* tag, Level level) {
        unsigned long ms = millis();
        unsigned long s  = (ms / 1000) % 60;
        unsigned long m  = (ms / 60000) % 60;
        unsigned long h  = (ms / 3600000) % 24;
        unsigned long d  = ms / 86400000UL;
        char ts[20];
        snprintf(ts, sizeof(ts), "[%03lu_%02lu%02lu%02lu]", d, h, m, s);

        const char* lvl = "DBG";
        if      (level == INFO)  lvl = "INF";
        else if (level == WARN)  lvl = "WRN";
        else if (level == ERROR) lvl = "ERR";

        char tag8[9]; snprintf(tag8, sizeof(tag8), "%-8s", tag);
        return String(ts) + "[" + lvl + "][" + tag8 + "] ";
    }
};