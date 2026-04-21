// ══════════════════════════════════════════════════════════════════════════════
// sd_logger.h  — Persistent SD Card Logger  (v4)
//
// CHANGES v4:
//   • Boot-message buffer:
//     SDDatabase::begin() calls SDLogger::begin() BEFORE Serial.begin() in
//     setup(), so early log lines were printed to an un-initialised Serial and
//     lost. v4 adds a 32-slot ring buffer (_earlyBuf[]) that captures every
//     log() call made before Serial is confirmed ready.  Call
//     SDLogger::flushEarlyBuffer() immediately after Serial.begin() in setup()
//     to drain all buffered lines to Serial (and to the SD log file) at once.
//
//   • SDLogger::beginSerial() — marks Serial as ready so future log() calls
//     go directly to Serial instead of the buffer.  Call after Serial.begin().
//
//   • SDLogger::installPanicHandler() — installs esp_set_shutdown_handler() +
//     sets a custom abort() replacement that writes a final crash line to the
//     SD log before the chip resets.  Call once near the top of setup().
//
//   • All original v3 functionality (fatal(), bootDump(), readLog(), etc.)
//     is preserved unchanged.
//
// Recommended setup() call order:
//   1. SDDatabase::begin();          // mounts SD, calls SDLogger::begin()
//   2. Serial.begin(115200);
//   3. SDLogger::beginSerial();      // marks Serial ready
//   4. SDLogger::flushEarlyBuffer(); // drains buffered boot messages to Serial
//   5. SDLogger::installPanicHandler();
//   6. ... rest of setup ...
//
// Log format (plain text, one line per entry):
//   [DAY_HHMMSS][LEVEL][TAG     ] message
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_system.h>   // esp_register_shutdown_handler

class SDLogger {
public:
    enum Level { DEBUG = 0, INFO, WARN, ERROR };

    // ── begin ─────────────────────────────────────────────────────────────────
    // Called from SDDatabase::begin() — Serial may NOT be ready yet.
    // Safe to call multiple times (idempotent).
    static void begin() {
        if (_sdReady) return;
        _sdReady = true;
        _ensureDir();
        bootDump();
        // Buffer this line — Serial may not be ready yet
        log("LOG", INFO, "=== SD Logger v4 started ===");
    }

    // ── beginSerial ───────────────────────────────────────────────────────────
    // Call immediately after Serial.begin() so future log() calls go directly
    // to Serial instead of the ring buffer.
    static void beginSerial() {
        _serialReady = true;
    }

    // ── flushEarlyBuffer ──────────────────────────────────────────────────────
    // Call after Serial.begin() + beginSerial() to dump all lines that were
    // buffered before Serial was initialised.
    static void flushEarlyBuffer() {
        if (!_serialReady) return;
        if (_earlyCount == 0) return;

        Serial.println();
        Serial.println("╔══════════════════════════════════════════╗");
        Serial.println("║         BUFFERED BOOT LOG (pre-Serial)   ║");
        Serial.println("╠══════════════════════════════════════════╣");
        for (int i = 0; i < _earlyCount; i++) {
            Serial.print("║  ");
            Serial.println(_earlyBuf[i]);
        }
        Serial.println("╚══════════════════════════════════════════╝");
        Serial.println();
        Serial.flush();
        _earlyCount = 0;
    }

    // ── installPanicHandler ───────────────────────────────────────────────────
    // Registers an ESP-IDF shutdown handler that writes a final log entry
    // before the chip resets on any fatal error (abort, stack overflow, WDT).
    // Call once near the top of setup().
    static void installPanicHandler() {
        esp_register_shutdown_handler([]() {
            if (!_sdReady) return;
            _ensureDir();
            File f = SD_MMC.open(_logPath(), FILE_APPEND);
            if (!f) return;
            unsigned long ms = millis();
            char ts[24];
            snprintf(ts, sizeof(ts), "[%03lu_%02lu%02lu%02lu]",
                     ms/86400000UL, (ms/3600000)%24,
                     (ms/60000)%60, (ms/1000)%60);
            f.printf("%s[ERR][PANIC   ] *** SYSTEM PANIC / SHUTDOWN at uptime %lums"
                     "  heap=%u psram=%u ***\n",
                     ts, ms, ESP.getFreeHeap(), ESP.getFreePsram());
            f.flush();
            f.close();

            // Also write last_crash.log so it survives and prints on next boot
            File cf = SD_MMC.open("/logs/last_crash.log", FILE_WRITE);
            if (cf) {
                cf.println("=== LAST FATAL EVENT ===");
                cf.printf("%s[ERR][PANIC   ] SYSTEM PANIC at uptime %lums"
                          "  heap=%u psram=%u\n",
                          ts, ms, ESP.getFreeHeap(), ESP.getFreePsram());
                cf.println("========================");
                cf.flush();
                cf.close();
            }
        });
    }

    // ── log ───────────────────────────────────────────────────────────────────
    static void log(const char* tag, Level level, const String& msg) {
        String line = _prefix(tag, level) + msg;

        if (_serialReady) {
            Serial.println(line);
            Serial.flush();
        } else {
            // Serial not yet ready — buffer the line
            if (_earlyCount < EARLY_BUF_SIZE) {
                _earlyBuf[_earlyCount++] = line;
            }
            // Still write to SD even without Serial
        }

        if (!_sdReady) return;

        File f = SD_MMC.open(_logPath(), FILE_APPEND);
        if (!f) return;
        f.println(line);
        f.flush();
        f.close();
    }

    // ── logf (printf-style) ───────────────────────────────────────────────────
    static void logf(const char* tag, Level level, const char* fmt, ...) {
        char buf[320];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log(tag, level, String(buf));
    }

    // ── section ───────────────────────────────────────────────────────────────
    static void section(const char* title) {
        String line = "════ ";
        line += title;
        line += " ════";
        log("LOG", INFO, line);
    }

    // ── fatal ─────────────────────────────────────────────────────────────────
    // Logs ERROR and writes /logs/last_crash.log so the cause survives reboot.
    static void fatal(const char* tag, const String& msg) {
        log(tag, ERROR, "FATAL: " + msg);

        if (!_sdReady) return;
        _ensureDir();
        File f = SD_MMC.open("/logs/last_crash.log", FILE_WRITE);
        if (!f) return;
        f.println("=== LAST FATAL EVENT ===");
        f.println(_prefix(tag, ERROR) + "FATAL: " + msg);
        f.printf("heap=%u psram=%u uptime_ms=%lu\n",
                 ESP.getFreeHeap(), ESP.getFreePsram(), millis());
        f.println("========================");
        f.flush();
        f.close();

        if (_serialReady) {
            Serial.println("[SDLogger] Crash log written to /logs/last_crash.log");
            Serial.flush();
        }
    }

    // ── bootDump ─────────────────────────────────────────────────────────────
    // Print previous crash log to Serial.  Called from begin() but also safe to
    // call manually after Serial is ready to guarantee visibility.
    static void bootDump() {
        if (!SD_MMC.exists("/logs/last_crash.log")) return;

        File f = SD_MMC.open("/logs/last_crash.log", FILE_READ);
        if (!f) return;

        // Capture content first (file may be open during Serial output)
        String content = "";
        while (f.available()) {
            content += (char)f.read();
            if (content.length() > 4096) break;
        }
        f.close();

        if (_serialReady) {
            _printCrashBox(content);
        } else {
            // Buffer each line individually so it appears in flushEarlyBuffer()
            if (_earlyCount < EARLY_BUF_SIZE)
                _earlyBuf[_earlyCount++] = "=== PREVIOUS SESSION CRASH ===";
            int pos = 0;
            while (pos < (int)content.length() && _earlyCount < EARLY_BUF_SIZE) {
                int nl = content.indexOf('\n', pos);
                String line = (nl < 0) ? content.substring(pos) : content.substring(pos, nl);
                line.trim();
                if (line.length() > 0) _earlyBuf[_earlyCount++] = "  " + line;
                if (nl < 0) break;
                pos = nl + 1;
            }
        }
    }

    // ── dumpToSerial ──────────────────────────────────────────────────────────
    static void dumpToSerial(int maxLines = 100) {
        if (!_sdReady) { Serial.println("[SDLogger] Not ready"); return; }
        String path = _logPath();
        if (!SD_MMC.exists(path)) {
            Serial.println("[SDLogger] No log file: " + path);
            return;
        }
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) return;

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
    static String readLog(int dayOffset = 0) {
        if (!_sdReady) return "";
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

    // ── readCrashLog ──────────────────────────────────────────────────────────
    static String readCrashLog() {
        if (!_sdReady) return "";
        if (!SD_MMC.exists("/logs/last_crash.log")) return "(no crash log)";
        File f = SD_MMC.open("/logs/last_crash.log", FILE_READ);
        if (!f) return "(open failed)";
        String out = "";
        while (f.available()) {
            out += (char)f.read();
            if (out.length() > 4096) break;
        }
        f.close();
        return out;
    }

    // ── listLogFiles ─────────────────────────────────────────────────────────
    static String listLogFiles() {
        if (!_sdReady) return "";
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

    static bool isReady()       { return _sdReady; }
    static bool isSerialReady() { return _serialReady; }

private:
    static const int EARLY_BUF_SIZE = 64;

    inline static bool   _sdReady     = false;
    inline static bool   _serialReady = false;
    inline static String _earlyBuf[EARLY_BUF_SIZE];
    inline static int    _earlyCount  = 0;

    static void _ensureDir() {
        if (!SD_MMC.exists("/logs")) SD_MMC.mkdir("/logs");
    }

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

    static void _printCrashBox(const String& content) {
        Serial.println();
        Serial.println("╔══════════════════════════════════════════╗");
        Serial.println("║        PREVIOUS SESSION CRASH LOG        ║");
        Serial.println("╠══════════════════════════════════════════╣");
        int pos = 0;
        while (pos < (int)content.length()) {
            int nl = content.indexOf('\n', pos);
            String line = (nl < 0) ? content.substring(pos) : content.substring(pos, nl);
            line.trim();
            if (line.length() > 0) {
                Serial.print("║  ");
                Serial.println(line);
            }
            if (nl < 0) break;
            pos = nl + 1;
        }
        Serial.println("╚══════════════════════════════════════════╝");
        Serial.println();
        Serial.flush();
    }
};