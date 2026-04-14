// ══════════════════════════════════════════════════════════════════════════════
// sd_file_manager.h  — SD Card File Operations
//
// Provides delete, rename, copy, and move for files and directories on
// the SD_MMC filesystem.  All operations are logged via SDLogger.
//
// Usage:
//   SDFileManager::deleteFile("/photos/17.jpg");
//   SDFileManager::renameFile("/logs/old.log", "/logs/new.log");
//   SDFileManager::copyFile("/employees/5.json", "/backup/5.json");
//   SDFileManager::moveFile("/tmp/photo.jpg", "/photos/42.jpg");
//   SDFileManager::deleteDir("/tmp");           // removes dir + all contents
//   SDFileManager::listDir("/photos", result);  // fills vector of FileInfo
//
// All methods return true on success, false on failure.
// All operations are logged to SDLogger (INFO on success, ERROR on failure).
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <SD_MMC.h>
#include "sd_logger.h"

// ── FileInfo ──────────────────────────────────────────────────────────────────
struct FileInfo {
    String  path;
    String  name;
    size_t  size      = 0;
    bool    isDir     = false;
};

// ── SDFileManager ─────────────────────────────────────────────────────────────
class SDFileManager {
public:

    // ── deleteFile ────────────────────────────────────────────────────────────
    // Remove a single file. Returns true if the file is gone afterwards.
    static bool deleteFile(const String& path) {
        SDLogger::log("FMGR", SDLogger::INFO,
                      "deleteFile: " + path);

        if (!SD_MMC.exists(path)) {
            SDLogger::log("FMGR", SDLogger::WARN,
                          "deleteFile: not found: " + path);
            return false;
        }

        // Refuse to delete directories with this function
        File f = SD_MMC.open(path);
        bool isDir = f && f.isDirectory();
        if (f) f.close();
        if (isDir) {
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "deleteFile: path is a directory, use deleteDir: " + path);
            return false;
        }

        bool ok = SD_MMC.remove(path);
        if (ok) {
            SDLogger::log("FMGR", SDLogger::INFO,
                          "deleteFile: OK: " + path);
        } else {
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "deleteFile: FAILED: " + path);
        }
        return ok;
    }

    // ── deleteDir ─────────────────────────────────────────────────────────────
    // Recursively delete a directory and all its contents.
    static bool deleteDir(const String& path) {
        SDLogger::log("FMGR", SDLogger::INFO,
                      "deleteDir: " + path);

        if (!SD_MMC.exists(path)) {
            SDLogger::log("FMGR", SDLogger::WARN,
                          "deleteDir: not found: " + path);
            return false;
        }

        // Recursively delete contents first
        _deleteDirContents(path);

        bool ok = SD_MMC.rmdir(path);
        if (ok) {
            SDLogger::log("FMGR", SDLogger::INFO,
                          "deleteDir: removed dir: " + path);
        } else {
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "deleteDir: rmdir FAILED: " + path);
        }
        return ok;
    }

    // ── renameFile ────────────────────────────────────────────────────────────
    // Rename (or move within same filesystem — same as move but explicit).
    // SD_MMC.rename() works across directories as long as it's the same card.
    static bool renameFile(const String& fromPath, const String& toPath) {
        SDLogger::logf("FMGR", SDLogger::INFO,
                       "renameFile: '%s' -> '%s'",
                       fromPath.c_str(), toPath.c_str());

        if (!SD_MMC.exists(fromPath)) {
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "renameFile: source not found: " + fromPath);
            return false;
        }

        // Ensure destination directory exists
        String destDir = _parentDir(toPath);
        if (destDir.length() > 1) _ensureDir(destDir);

        bool ok = SD_MMC.rename(fromPath, toPath);
        if (ok) {
            SDLogger::logf("FMGR", SDLogger::INFO,
                           "renameFile: OK '%s' -> '%s'",
                           fromPath.c_str(), toPath.c_str());
        } else {
            SDLogger::logf("FMGR", SDLogger::ERROR,
                           "renameFile: FAILED '%s' -> '%s'",
                           fromPath.c_str(), toPath.c_str());
        }
        return ok;
    }

    // ── copyFile ──────────────────────────────────────────────────────────────
    // Copy a file byte-for-byte. Source is unchanged.
    // Copies in 4 KB chunks to keep WDT happy on large files.
    static bool copyFile(const String& srcPath, const String& dstPath) {
        SDLogger::logf("FMGR", SDLogger::INFO,
                       "copyFile: '%s' -> '%s'",
                       srcPath.c_str(), dstPath.c_str());

        if (!SD_MMC.exists(srcPath)) {
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "copyFile: source not found: " + srcPath);
            return false;
        }

        File src = SD_MMC.open(srcPath, FILE_READ);
        if (!src) {
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "copyFile: cannot open source: " + srcPath);
            return false;
        }

        size_t totalSize = src.size();

        // Ensure dest directory exists
        String destDir = _parentDir(dstPath);
        if (destDir.length() > 1) _ensureDir(destDir);

        // Remove existing dest file
        if (SD_MMC.exists(dstPath)) SD_MMC.remove(dstPath);

        File dst = SD_MMC.open(dstPath, FILE_WRITE);
        if (!dst) {
            src.close();
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "copyFile: cannot open dest for write: " + dstPath);
            return false;
        }

        const size_t CHUNK = 4096;
        uint8_t* buf = (uint8_t*)malloc(CHUNK);
        if (!buf) {
            src.close(); dst.close();
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "copyFile: malloc FAILED");
            return false;
        }

        size_t copied = 0;
        bool   ok     = true;
        while (src.available()) {
            size_t rd = src.read(buf, CHUNK);
            if (rd == 0) break;
            size_t wr = dst.write(buf, rd);
            if (wr != rd) { ok = false; break; }
            copied += wr;
            yield();
        }
        free(buf);
        src.close();
        dst.flush();
        dst.close();

        if (!ok || copied != totalSize) {
            SDLogger::logf("FMGR", SDLogger::ERROR,
                           "copyFile: FAILED after %u/%u bytes",
                           (unsigned)copied, (unsigned)totalSize);
            SD_MMC.remove(dstPath);
            return false;
        }

        SDLogger::logf("FMGR", SDLogger::INFO,
                       "copyFile: OK %u bytes '%s' -> '%s'",
                       (unsigned)copied, srcPath.c_str(), dstPath.c_str());
        return true;
    }

    // ── moveFile ──────────────────────────────────────────────────────────────
    // Move = rename (atomic on FAT32) with a copy+delete fallback.
    static bool moveFile(const String& srcPath, const String& dstPath) {
        SDLogger::logf("FMGR", SDLogger::INFO,
                       "moveFile: '%s' -> '%s'",
                       srcPath.c_str(), dstPath.c_str());

        // Try atomic rename first
        String destDir = _parentDir(dstPath);
        if (destDir.length() > 1) _ensureDir(destDir);

        bool ok = SD_MMC.rename(srcPath, dstPath);
        if (ok) {
            SDLogger::logf("FMGR", SDLogger::INFO,
                           "moveFile: rename OK '%s' -> '%s'",
                           srcPath.c_str(), dstPath.c_str());
            return true;
        }

        // Fallback: copy + delete
        SDLogger::log("FMGR", SDLogger::WARN,
                      "moveFile: rename failed, trying copy+delete");
        if (!copyFile(srcPath, dstPath)) return false;
        ok = SD_MMC.remove(srcPath);
        if (ok) {
            SDLogger::logf("FMGR", SDLogger::INFO,
                           "moveFile: copy+delete OK '%s' -> '%s'",
                           srcPath.c_str(), dstPath.c_str());
        } else {
            SDLogger::logf("FMGR", SDLogger::ERROR,
                           "moveFile: copy OK but delete FAILED for source: %s",
                           srcPath.c_str());
        }
        return ok;
    }

    // ── ensureDir (public) ────────────────────────────────────────────────────
    // Create a directory (and any missing parents).
    static bool ensureDir(const String& path) {
        return _ensureDir(path);
    }

    // ── listDir ───────────────────────────────────────────────────────────────
    // Fill `results` (max `maxEntries`) with entries in `path`.
    // Returns total number of entries found (may exceed maxEntries).
    static int listDir(const String& path,
                       FileInfo* results, int maxEntries) {
        if (!SD_MMC.exists(path)) return 0;
        File dir = SD_MMC.open(path);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return 0; }

        int count = 0;
        File entry = dir.openNextFile();
        while (entry) {
            if (count < maxEntries) {
                results[count].name  = String(entry.name());
                results[count].path  = path + "/" + results[count].name;
                results[count].size  = entry.isDirectory() ? 0 : entry.size();
                results[count].isDir = entry.isDirectory();
            }
            count++;
            entry.close();
            entry = dir.openNextFile();
            yield();
        }
        dir.close();
        return count;
    }

    // ── listDirJson ───────────────────────────────────────────────────────────
    // Return directory listing as a JSON string (for web portal API).
    static String listDirJson(const String& path) {
        if (!SD_MMC.exists(path)) return "{\"error\":\"path not found\"}";
        File dir = SD_MMC.open(path);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            return "{\"error\":\"not a directory\"}";
        }

        String json = "{\"path\":\"" + _jsonEsc(path) + "\",\"entries\":[";
        bool first = true;
        File entry = dir.openNextFile();
        int count = 0;
        while (entry && count < 200) {
            if (!first) json += ",";
            first = false;
            String n = String(entry.name());
            bool isD = entry.isDirectory();
            size_t sz = isD ? 0 : entry.size();
            json += "{\"name\":\"" + _jsonEsc(n) + "\""
                 +  ",\"path\":\"" + _jsonEsc(path + "/" + n) + "\""
                 +  ",\"size\":" + String(sz)
                 +  ",\"dir\":" + (isD ? "true" : "false")
                 +  "}";
            entry.close();
            entry = dir.openNextFile();
            count++;
            yield();
        }
        dir.close();
        json += "],\"count\":" + String(count) + "}";
        return json;
    }

    // ── fileExists ────────────────────────────────────────────────────────────
    static bool fileExists(const String& path) {
        return SD_MMC.exists(path);
    }

    // ── fileSize ──────────────────────────────────────────────────────────────
    static size_t fileSize(const String& path) {
        if (!SD_MMC.exists(path)) return 0;
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) return 0;
        size_t sz = f.size();
        f.close();
        return sz;
    }

    // ── readTextFile ──────────────────────────────────────────────────────────
    // Read entire text file into a String (max 64 KB).
    static String readTextFile(const String& path) {
        if (!SD_MMC.exists(path)) return "";
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) return "";
        String out = "";
        while (f.available() && out.length() < 65536)
            out += (char)f.read();
        f.close();
        return out;
    }

    // ── writeTextFile ─────────────────────────────────────────────────────────
    // Overwrite (or create) a text file with the given content.
    static bool writeTextFile(const String& path, const String& content) {
        String dir = _parentDir(path);
        if (dir.length() > 1) _ensureDir(dir);
        if (SD_MMC.exists(path)) SD_MMC.remove(path);
        File f = SD_MMC.open(path, FILE_WRITE);
        if (!f) {
            SDLogger::log("FMGR", SDLogger::ERROR,
                          "writeTextFile: open FAILED: " + path);
            return false;
        }
        size_t written = f.print(content);
        f.flush(); f.close();
        SDLogger::logf("FMGR", SDLogger::INFO,
                       "writeTextFile: wrote %u B to %s",
                       (unsigned)written, path.c_str());
        return (written == content.length());
    }

private:
    // ── _ensureDir ────────────────────────────────────────────────────────────
    static bool _ensureDir(const String& path) {
        if (path.length() <= 1) return true;
        if (SD_MMC.exists(path)) return true;

        // Create parent first
        String parent = _parentDir(path);
        if (parent.length() > 1 && !SD_MMC.exists(parent))
            _ensureDir(parent);

        bool ok = SD_MMC.mkdir(path);
        if (ok) SDLogger::log("FMGR", SDLogger::INFO, "mkdir: " + path);
        else    SDLogger::log("FMGR", SDLogger::ERROR, "mkdir FAILED: " + path);
        return ok;
    }

    // ── _deleteDirContents ────────────────────────────────────────────────────
    static void _deleteDirContents(const String& path) {
        File dir = SD_MMC.open(path);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }

        File entry = dir.openNextFile();
        while (entry) {
            String entryPath = path + "/" + String(entry.name());
            bool isDir = entry.isDirectory();
            entry.close();
            if (isDir) {
                _deleteDirContents(entryPath);
                SD_MMC.rmdir(entryPath);
                SDLogger::log("FMGR", SDLogger::INFO, "rmdir: " + entryPath);
            } else {
                SD_MMC.remove(entryPath);
                SDLogger::log("FMGR", SDLogger::INFO, "rm: " + entryPath);
            }
            entry = dir.openNextFile();
            yield();
        }
        dir.close();
    }

    // ── _parentDir ────────────────────────────────────────────────────────────
    static String _parentDir(const String& path) {
        int idx = path.lastIndexOf('/');
        if (idx <= 0) return "/";
        return path.substring(0, idx);
    }

    // ── _jsonEsc ──────────────────────────────────────────────────────────────
    static String _jsonEsc(const String& s) {
        String out = "";
        for (unsigned int i = 0; i < s.length(); i++) {
            char c = s[i];
            if      (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else                out += c;
        }
        return out;
    }
};




















