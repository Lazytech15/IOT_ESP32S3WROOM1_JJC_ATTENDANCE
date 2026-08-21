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
#include <SPI.h>
#include <SD.h>
#include <esp_system.h>   // esp_register_shutdown_handler
#include <time.h>         // wall-clock timestamp for crash logs
#include "sd_mutex.h"

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

    // ── suspendSDWrite ───────────────────────────────────────────────────────
    // Skips the per-line SD_MMC.open/append/flush/close AND the Serial.println
    // in log() while suspended, for DEBUG/INFO/WARN (ERROR always still goes
    // to both — see log()). Each SD file open/close is several ms, and each
    // Serial.println is CPU time on Core 0 shared with the higher-priority
    // nfcWorker task; a seed pass fires ~60 trace lines per employee, so
    // across 44-47 employees that's ~2800+ lines of overhead on both fronts.
    // These are debug traces, not audit data (the actual attendance
    // CSV/reconcile results are unaffected either way — they're written via
    // separate SD_MMC calls, not through this logger), so the seed passes
    // suspend both outputs for just their noisy per-employee loops and
    // resume afterward. Nest-safe via a counter, not a bool, so a pass
    // calling another logged helper doesn't accidentally re-enable mid-way.
    static void suspendSDWrite(bool suspend) {
        if (suspend) _sdWriteSuspendDepth++;
        else if (_sdWriteSuspendDepth > 0) _sdWriteSuspendDepth--;
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
    //
    // IMPORTANT: esp_register_shutdown_handler() callbacks run on EVERY
    // esp_restart() — including deliberate ones (portal reboot button, OTA,
    // etc.) — not only on genuine crashes. Any code path that calls
    // ESP.restart() intentionally MUST call markIntentionalRestart() first,
    // or this handler will mislabel a normal reboot as "SYSTEM PANIC" in
    // last_crash.log.
    static void installPanicHandler() {
        esp_register_shutdown_handler([]() {
            if (_intentionalRestart) return;   // deliberate reboot — not a crash, skip
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

            // Also write last_crash.log so it survives and prints on next boot.
            // Include a wall-clock timestamp (when available) so a reader can
            // tell at a glance whether this crash is from tonight or from
            // three weeks ago, instead of only a millis()-since-boot value
            // that resets to near-zero on every restart.
            String wallTime = _wallClockStr();
            File cf = SD_MMC.open("/logs/last_crash.log", FILE_WRITE);
            if (cf) {
                cf.println("=== LAST FATAL EVENT ===");
                if (wallTime.length())
                    cf.printf("Crash time: %s\n", wallTime.c_str());
                else
                    cf.println("Crash time: unknown (clock not yet synced at crash time)");
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
    // MAX_LOG_BYTES caps a single log file before it's rotated. Previously
    // unbounded — _logPath() is keyed off days-since-boot, so a device that
    // rarely reboots kept appending to the SAME file forever (observed at
    // 3.54MB and climbing with no ceiling). One rotated backup (.1) is kept;
    // anything older is discarded — these are diagnostic logs, not records
    // that need indefinite retention (attendance data has its own separate
    // purge path in main.cpp).
    static const uint32_t MAX_LOG_BYTES = 2UL * 1024 * 1024;   // 2MB per file

    static void _rotateIfNeeded(const String& path) {
        if (!SD_MMC.exists(path)) return;
        File chk = SD_MMC.open(path, FILE_READ);
        if (!chk) return;
        size_t sz = chk.size();
        chk.close();
        if (sz < MAX_LOG_BYTES) return;

        String oldPath = path + ".1";
        if (SD_MMC.exists(oldPath)) SD_MMC.remove(oldPath);
        SD_MMC.rename(path, oldPath);
    }

    static void log(const char* tag, Level level, const String& msg) {
        String line = _prefix(tag, level) + msg;

        // ── Suspend-aware output ─────────────────────────────────────────
        // suspendSDWrite() marks a burst of noisy per-employee DEBUG/INFO
        // tracing (seed/reconcile passes: ~60 lines x each employee). Serial
        // used to still print every one of those lines even while suspended
        // — only the SD file write was skipped. Two costs from that:
        //   1. ~2800+ Serial.println() calls back-to-back on a 47-employee
        //      pass, each consuming CPU time on Core 0 — the same core (and
        //      lower priority) as nfcWorker, which is what actually wakes
        //      the screen and answers an NFC tap. That competition is a
        //      real, if usually small, source of tap-to-wake latency during
        //      exactly the moments a seed/reconcile pass is mid-flight.
        //   2. A terminal reading that much UART traffic that fast visibly
        //      garbles/truncates lines (interleaved writes, dropped bytes).
        // DEBUG/INFO/WARN are now silenced on both Serial and SD together
        // while suspended — ERROR always gets through on both, unchanged
        // from before, since a real failure must never go unseen.
        bool suspended = (_sdWriteSuspendDepth > 0 && level != ERROR);

        if (!suspended) {
            if (_serialReady) {
                Serial.println(line);
                // NOTE: Serial.flush() removed — it blocked until every byte was
                // physically clocked out over the 115200-baud UART before this
                // call could return. With ~60 log lines fired per employee across
                // a 44-employee seed pass, that blocking wait (not the actual SD
                // write below) was the single biggest contributor to a seed pass
                // taking 50-67s instead of a few seconds. The UART's own buffer
                // still drains asynchronously in the background — nothing is lost,
                // it just no longer stalls the caller.
            } else {
                // Serial not yet ready — buffer the line
                if (_earlyCount < EARLY_BUF_SIZE) {
                    _earlyBuf[_earlyCount++] = line;
                }
                // Still write to SD even without Serial
            }
        }

        // ── ERROR priority bypass ────────────────────────────────────────
        // ERROR always reaches the SD log regardless of suspend state, and
        // is additionally mirrored to a small dedicated priority file (see
        // _writePriorityError) so a fatal reboot moments later can't take
        // the only copy of the error with it.
        if (level == ERROR) {
            _writePriorityError(line);
        }

        if (!_sdReady) return;
        if (suspended) return;

        // Locked: this fires ~60x per employee during a seed/reconcile pass
        // (see comment above) — without a lock this was the single biggest
        // source of SD_MMC contention with an in-flight NFC tap, even more
        // than the seed snapshot file itself. See sd_mutex.h.
        SDLockGuard _sdLock;
        String path = _logPath();
        _rotateIfNeeded(path);

        File f = SD_MMC.open(path, FILE_APPEND);
        if (!f) return;
        f.println(line);
        f.flush();
        f.close();
    }

    // ── _writePriorityError ─────────────────────────────────────────────────
    // Every ERROR-level log() call lands here immediately, independent of
    // suspendSDWrite() and independent of the main rotating log file. Purpose:
    // a device that panics/reboots shortly after an error should still have
    // that error recoverable on the next boot, even if the main log write for
    // that same line got skipped for some other reason, or the panic happened
    // before the main log's rotate/flush completed. Kept small (256KB cap,
    // one .1 backup) since real errors should be rare — this is meant to be
    // opened and read by a human debugging a specific incident, not scanned
    // programmatically like the main log.
    static void _writePriorityError(const String& line) {
        if (!_sdReady) return;
        SDLockGuard _sdLock;   // serialize SD_MMC access across tasks — see sd_mutex.h
        _ensureDir();

        const char* path = "/logs/priority_errors.log";
        const uint32_t MAX_PRIORITY_BYTES = 256UL * 1024;   // 256KB

        if (SD_MMC.exists(path)) {
            File chk = SD_MMC.open(path, FILE_READ);
            if (chk) {
                size_t sz = chk.size();
                chk.close();
                if (sz >= MAX_PRIORITY_BYTES) {
                    String oldPath = String(path) + ".1";
                    if (SD_MMC.exists(oldPath)) SD_MMC.remove(oldPath);
                    SD_MMC.rename(path, oldPath);
                }
            }
        }

        File f = SD_MMC.open(path, FILE_APPEND);
        if (!f) return;
        String wallTime = _wallClockStr();
        f.printf("[%s] uptime=%lums heap=%u psram=%u\n",
                  wallTime.length() ? wallTime.c_str() : "clock not synced",
                  millis(), ESP.getFreeHeap(), ESP.getFreePsram());
        f.println(line);
        f.flush();   // always flushed immediately — this file exists
                     // specifically to survive a crash moments later
        f.close();
    }

    // ── logf (printf-style) ───────────────────────────────────────────────────
    static void logf(const char* tag, Level level, const char* fmt, ...) {
        if (!fmt) return;
        if (!tag) tag = "???";
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
        String wallTime = _wallClockStr();
        if (wallTime.length())
            f.printf("Crash time: %s\n", wallTime.c_str());
        else
            f.println("Crash time: unknown (clock not yet synced at crash time)");
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

    // ── resetReasonStr ───────────────────────────────────────────────────────
    // esp_reset_reason() is only meaningful AFTER a reboot (it reads a value
    // latched in RTC memory), which is why installPanicHandler()'s shutdown
    // callback can't log it — at that point the chip hasn't reset yet. This
    // is the missing half: called on the NEXT boot, it says *why* the
    // previous session ended (task watchdog, brownout, panic/exception,
    // etc.) instead of the old crash log's bare "SYSTEM PANIC" with no cause.
    static const char* resetReasonStr() {
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:   return "POWERON (cold boot)";
            case ESP_RST_EXT:       return "EXT (external reset pin)";
            case ESP_RST_SW:        return "SW (esp_restart() called)";
            case ESP_RST_PANIC:     return "PANIC (unhandled exception / abort)";
            case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog — ISR ran too long)";
            case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog — a task blocked/looped too long)";
            case ESP_RST_WDT:       return "WDT (other watchdog)";
            case ESP_RST_DEEPSLEEP: return "DEEPSLEEP wake";
            case ESP_RST_BROWNOUT:  return "BROWNOUT (voltage dip — check power supply)";
            case ESP_RST_SDIO:      return "SDIO";
            default:                return "UNKNOWN";
        }
    }

    // ── bootDump ─────────────────────────────────────────────────────────────
    // Print previous crash log to Serial.  Called from begin() but also safe to
    // call manually after Serial is ready to guarantee visibility.
    static void bootDump() {
        // Log the cause of THIS boot regardless of whether a crash log exists —
        // e.g. TASK_WDT/BROWNOUT/PANIC vs a normal POWERON/SW restart. This is
        // buffered like any other log() call, so it shows up in the early
        // buffer dump even though Serial may not be ready yet.
        log("BOOT", (esp_reset_reason() == ESP_RST_POWERON ||
                     esp_reset_reason() == ESP_RST_SW) ? INFO : ERROR,
            String("Reset reason: ") + resetReasonStr());

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
                _earlyBuf[_earlyCount++] = "=== CRASH LOG FROM A PREVIOUS BOOT SESSION ===";
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
        SDLockGuard _sdLock;   // serialize SD_MMC access across tasks — see sd_mutex.h
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
        SDLockGuard _sdLock;   // serialize SD_MMC access across tasks — see sd_mutex.h
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
        SDLockGuard _sdLock;   // serialize SD_MMC access across tasks — see sd_mutex.h
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

    // ── markIntentionalRestart ──────────────────────────────────────────────
    // Call this immediately before any deliberate ESP.restart() (portal
    // reboot button, OTA, config-apply, etc.) so installPanicHandler()'s
    // shutdown callback can tell "the user asked for this" apart from a
    // genuine abort/panic/watchdog reset — both fire the same shutdown
    // handler, since esp_register_shutdown_handler() runs on ANY
    // esp_restart(), not only fatal ones. Without this, every normal reboot
    // was being mislabeled as "SYSTEM PANIC" in last_crash.log.
    static void markIntentionalRestart() { _intentionalRestart = true; }

private:
    static const int EARLY_BUF_SIZE = 64;

    inline static bool   _sdReady     = false;
    inline static bool   _serialReady = false;
    inline static bool   _intentionalRestart = false;
    inline static int    _sdWriteSuspendDepth = 0;
    inline static String _earlyBuf[EARLY_BUF_SIZE];
    inline static int    _earlyCount  = 0;

    static void _ensureDir() {
        if (!SD_MMC.exists("/logs")) SD_MMC.mkdir("/logs");
    }

    // ── _wallClockStr ─────────────────────────────────────────────────────────
    // Returns "YYYY-MM-DD HH:MM:SS" if the system clock has been set (NTP/RTC
    // sync completed at least once since power-on), or "" otherwise. Used to
    // stamp crash records with an actual date instead of only millis()-since-
    // boot, so a stale crash log from days ago isn't mistaken for a fresh one.
    // Threshold of 1600000000 (Sep 2020) is a cheap "has this clock ever been
    // set" check — an un-synced ESP32 clock starts at/near epoch 0.
    static String _wallClockStr() {
        time_t now = time(nullptr);
        if (now < 1600000000) return "";
        struct tm tmInfo;
        localtime_r(&now, &tmInfo);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
        return String(buf);
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
        Serial.println("║   CRASH LOG FROM A PREVIOUS BOOT SESSION ║");
        Serial.println("║   (persists on SD until the next crash)  ║");
        Serial.println("╠══════════════════════════════════════════╣");
        // content already contains its own "Crash time: ..." line (or the
        // "unknown (clock not yet synced)" fallback) written at crash time,
        // so the box itself carries enough info to tell whether this is
        // fresh or stale — no need to re-derive it here.
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