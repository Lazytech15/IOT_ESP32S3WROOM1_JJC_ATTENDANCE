// ══════════════════════════════════════════════════════════════════════════════
// employee_profile_display.h  — Portrait 240×320
//
// NEW LAYOUT (photo-first, compact info):
//
//   ┌──────────────────────────────┐  y=0
//   │                              │
//   │    ░░░░░ PHOTO / JPEG ░░░░░  │  cx=120, cy=95, r=82  (164px circle)
//   │                              │
//   ├──────────────────────────────┤  y=185
//   │  Full Name (font 2)          │  y=192
//   │  ID: XXXXX   (font 1)        │  y=208
//   │  Position    (font 1)        │  y=220
//   │  [ Dept badge ]              │  y=233
//   ├──────────────────────────────┤  y=250
//   │  ╔══════ CLOCK IN/OUT ═════╗ │  y=256..310
//   │  ╚════════════════════════╝  │
//   └──────────────────────────────┘  y=320
//
// JPEG rendering: TJpgDec (bundled with TFT_eSPI).
// Add to platformio.ini:
//   lib_deps = Bodmer/TJpg_Decoder
//
// The profilePicture field holds the SERVER-RELATIVE path returned by the PHP
// API (e.g. "uploads/photos/17.jpg").  The HTTP service uses it to build the
// full download URL.  The SD cache stores the file at /photos/<uid>.jpg.
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SD_MMC.h>
#include "TFTDisplayManager.h"
#include <TJpg_Decoder.h>

#ifndef SCREEN_W
#define SCREEN_W 240
#endif
#ifndef SCREEN_H
#define SCREEN_H 320
#endif

// ══════════════════════════════════════════════════════════════════════════════
// EmployeeProfile
// ══════════════════════════════════════════════════════════════════════════════
struct EmployeeProfile {
    String uid             = "";
    String idNumber        = "";
    String fullName        = "";
    String firstName       = "";
    String lastName        = "";
    String position        = "";
    String department      = "";
    String email           = "";
    String status          = "Active";
    String employmentType  = "";
    String authTime        = "";
    String clockType       = "check-in"; // "check-in"|"check-out"|"denied"
    // profilePicture: server-relative path, e.g. "uploads/photos/17.jpg"
    // Used by AttendanceHTTPService to build the download URL.
    String profilePicture  = "";
    bool   accessGranted   = false;
    bool   hasData         = false;
};

// ══════════════════════════════════════════════════════════════════════════════
// TJpgDec output callback — pushes decoded rows straight to TFT
// ══════════════════════════════════════════════════════════════════════════════
static int16_t   _jpegX   = 0;
static int16_t   _jpegY   = 0;
static TFT_eSPI* _jpegTft = nullptr;

static bool _jpegCB(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
    if (!_jpegTft) return false;
    _jpegTft->pushImage(_jpegX + x, _jpegY + y, w, h, bmp);
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// EmployeeProfileDisplay
// ══════════════════════════════════════════════════════════════════════════════
class EmployeeProfileDisplay {
public:
    EmployeeProfileDisplay() {
        _tft = TFTDisplayManager::getTFT();
        TJpgDec.setJpgScale(1);
        TJpgDec.setCallback(_jpegCB);
        TJpgDec.setSwapBytes(true);  // ESP32 LE → TFT BE swap
    }

    // ─────────────────────────────────────────────────────────────────────────
    void showLoading() {
        if (!_tft) return;
        _tft->fillScreen(TFTColors::BG_DARK);
        int cx = SCREEN_W / 2, cy = SCREEN_H / 2 - 30;
        _tft->drawCircle(cx, cy, 34, TFTColors::ACCENT_TEAL);
        _tft->fillCircle(cx, cy, 30, TFTColors::BG_MID);
        _tft->setTextColor(TFTColors::ACCENT_TEAL, TFTColors::BG_MID);
        _tft->setTextDatum(MC_DATUM);
        _tft->drawString("...", cx, cy, 4);
        _tft->setTextColor(TFTColors::TEXT_SECONDARY, TFTColors::BG_DARK);
        _tft->drawString("Looking up card...", SCREEN_W / 2, cy + 55, 2);
        _tft->setTextDatum(TL_DATUM);
    }

    // ─────────────────────────────────────────────────────────────────────────
    void showError(const String& message) {
        if (!_tft) return;
        _tft->fillScreen(TFTColors::BG_DARK);
        int cx = SCREEN_W / 2, cy = 120;
        _tft->fillCircle(cx, cy, 40, TFTColors::ERROR);
        _tft->setTextColor(TFTColors::WHITE, TFTColors::ERROR);
        _tft->setTextDatum(MC_DATUM);
        _tft->drawString("X", cx, cy, 6);
        _tft->setTextColor(TFTColors::ERROR, TFTColors::BG_DARK);
        _tft->drawString("ACCESS DENIED", SCREEN_W / 2, 185, 2);
        _tft->setTextColor(TFTColors::TEXT_SECONDARY, TFTColors::BG_DARK);
        String m = message; if (m.length() > 26) m = m.substring(0, 26);
        _tft->drawString(m, SCREEN_W / 2, 205, 1);
        _tft->setTextDatum(TL_DATUM);
    }

    // ─────────────────────────────────────────────────────────────────────────
    void showSuccess(const String& name) {
        if (!_tft) return;
        _tft->fillScreen(TFTColors::BG_DARK);
        int cx = SCREEN_W / 2, cy = 120;
        _tft->fillCircle(cx, cy, 40, TFTColors::SUCCESS);
        _tft->setTextColor(TFTColors::BLACK, TFTColors::SUCCESS);
        _tft->setTextDatum(MC_DATUM);
        _tft->drawString("OK", cx, cy, 4);
        _tft->setTextColor(TFTColors::SUCCESS, TFTColors::BG_DARK);
        _tft->drawString("ATTENDANCE RECORDED", SCREEN_W / 2, 183, 1);
        _tft->setTextColor(TFTColors::TEXT_PRIMARY, TFTColors::BG_DARK);
        String n = name;
        while (n.length() > 1 && _tft->textWidth(n.c_str(), 2) > SCREEN_W - 20)
            n.remove(n.length() - 1);
        _tft->drawString(n, SCREEN_W / 2, 200, 2);
        _tft->setTextDatum(TL_DATUM);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Main profile card — photo-first, compact info strip below
    // photoPath = "/photos/<uid>.jpg" on SD card, or "" for initials fallback
    // ─────────────────────────────────────────────────────────────────────────
    void showEmployeeProfile(const EmployeeProfile& emp,
                              const String& photoPath = "") {
        if (!_tft) return;
        _tft->fillScreen(TFTColors::BG_DARK);

        bool isCheckIn   = (emp.clockType != "check-out");
        uint16_t brdCol  = isCheckIn ? TFTColors::SUCCESS : TFTColors::ERROR;

        // ── Double-line card border ───────────────────────────────────────────
        _tft->drawRoundRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, 12, brdCol);
        _tft->drawRoundRect(3, 3, SCREEN_W - 6, SCREEN_H - 6, 10, brdCol);

        // ── Photo / avatar  (large: r=82, top half of screen) ─────────────────
        const int PH_CX = SCREEN_W / 2;   // 120
        const int PH_CY = 95;              // vertically centred in top 185px
        const int PH_R  = 80;              // 160px diameter

        bool photoOk = false;
        if (photoPath.length() > 0)
            photoOk = _drawCircularJpeg(photoPath, PH_CX, PH_CY, PH_R, brdCol);
        if (!photoOk)
            _drawInitials(emp, PH_CX, PH_CY, PH_R, brdCol);

        // Ring border on top of photo
        _tft->drawCircle(PH_CX, PH_CY, PH_R + 2, brdCol);
        _tft->drawCircle(PH_CX, PH_CY, PH_R + 3, brdCol);

        // ── Divider below photo ───────────────────────────────────────────────
        const int INFO_Y = PH_CY + PH_R + 8;  // ≈ 185
        _tft->drawFastHLine(8, INFO_Y, SCREEN_W - 16, TFTColors::BORDER_DIM);

        // ── Name ──────────────────────────────────────────────────────────────
        _tft->setTextDatum(MC_DATUM);
        _tft->setTextColor(TFTColors::WHITE, TFTColors::BG_DARK);
        String nm = emp.fullName.length() ? emp.fullName
                                          : (emp.firstName + " " + emp.lastName);
        while (nm.length() > 1 && _tft->textWidth(nm.c_str(), 2) > SCREEN_W - 20)
            nm.remove(nm.length() - 1);
        _tft->drawString(nm, SCREEN_W / 2, INFO_Y + 10, 2);

        // ── ID ────────────────────────────────────────────────────────────────
        _tft->setTextColor(TFTColors::ACCENT_CYAN, TFTColors::BG_DARK);
        _tft->drawString("ID: " + emp.idNumber, SCREEN_W / 2, INFO_Y + 25, 1);

        // ── Position ──────────────────────────────────────────────────────────
        if (emp.position.length()) {
            _tft->setTextColor(TFTColors::TEXT_SECONDARY, TFTColors::BG_DARK);
            String pos = emp.position;
            while (pos.length() > 1 && _tft->textWidth(pos.c_str(), 1) > SCREEN_W - 22)
                pos.remove(pos.length() - 1);
            _tft->drawString(pos, SCREEN_W / 2, INFO_Y + 37, 1);
        }

        // ── Dept badge ────────────────────────────────────────────────────────
        if (emp.department.length()) {
            int bw = _tft->textWidth(emp.department.c_str(), 1) + 12;
            int bh = 15, bx = (SCREEN_W - bw) / 2, by = INFO_Y + 48;
            _tft->fillRoundRect(bx, by, bw, bh, 4, TFTColors::BG_LIGHT);
            _tft->drawRoundRect(bx, by, bw, bh, 4, TFTColors::BORDER_DIM);
            _tft->setTextColor(TFTColors::TEXT_DIM, TFTColors::BG_LIGHT);
            _tft->drawString(emp.department, SCREEN_W / 2, by + bh / 2, 1);
        }

        _tft->setTextDatum(TL_DATUM);

        // ── CLOCK IN / CLOCK OUT button ───────────────────────────────────────
        const int BTN_Y  = SCREEN_H - 66;
        const int BTN_X  = 12;
        const int BTN_W  = SCREEN_W - 24;
        const int BTN_H  = 52;

        uint16_t btnFill = isCheckIn ? TFTColors::SUCCESS : TFTColors::ERROR;
        _tft->fillRoundRect(BTN_X,     BTN_Y,     BTN_W,     BTN_H,     10, brdCol);
        _tft->fillRoundRect(BTN_X + 2, BTN_Y + 2, BTN_W - 4, BTN_H - 4,  8, btnFill);
        _tft->setTextDatum(MC_DATUM);
        _tft->setTextColor(TFTColors::BLACK, btnFill);
        _tft->drawString(isCheckIn ? "CLOCK IN" : "CLOCK OUT",
                         SCREEN_W / 2, BTN_Y + BTN_H / 2, 4);
        _tft->setTextDatum(TL_DATUM);

        Serial.println("[Profile] " + emp.fullName + " [" + emp.clockType +
                       "] photo=" + (photoOk ? "JPEG" : "initials"));
    }

    // ── stub preserved for compilation compatibility ──────────────────────────
    bool downloadPhoto(const String& uid, uint8_t** b, size_t* l) {
        (void)uid; (void)b; (void)l; return false;
    }

private:
    TFT_eSPI* _tft = nullptr;

    // ── Initials avatar ───────────────────────────────────────────────────────
    void _drawInitials(const EmployeeProfile& emp, int cx, int cy, int r,
                       uint16_t accent) {
        _tft->fillCircle(cx, cy, r, TFTColors::BG_MID);
        String ini;
        if (emp.firstName.length())  ini += (char)toupper(emp.firstName[0]);
        if (emp.lastName.length())   ini += (char)toupper(emp.lastName[0]);
        if (!ini.length() && emp.fullName.length())
            ini = String((char)toupper(emp.fullName[0]));
        _tft->setTextColor(accent, TFTColors::BG_MID);
        _tft->setTextDatum(MC_DATUM);
        _tft->drawString(ini, cx, cy, 6);   // font 6 = large
        _tft->setTextDatum(TL_DATUM);
    }

    // ── Circular JPEG from SD ─────────────────────────────────────────────────
    // Decode JPEG at bounding box top-left = (cx-r, cy-r), then mask corners.
    bool _drawCircularJpeg(const String& path, int cx, int cy, int r,
                           uint16_t accent) {
        if (!_tft) return false;

        // ── Existence check ───────────────────────────────────────────────────
        if (!SD_MMC.exists(path)) {
            Serial.println("[IMG] Not on SD: " + path);
            return false;
        }

        // ── Open + size check ─────────────────────────────────────────────────
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) { Serial.println("[IMG] Open failed: " + path); return false; }
        size_t fsz = f.size();
        if (fsz == 0 || fsz > 2000000) {  // raised from 300 KB → 2 MB
            Serial.printf("[IMG] Bad size %u: %s\n", fsz, path.c_str());
            f.close(); return false;
        }

        // ── Read into PSRAM / heap ────────────────────────────────────────────
        uint8_t* buf = (uint8_t*)ps_malloc(fsz);
        if (!buf)  buf = (uint8_t*)malloc(fsz);
        if (!buf) { Serial.println("[IMG] malloc fail"); f.close(); return false; }

        size_t got = f.read(buf, fsz);
        f.close();

        if (got != fsz) {
            Serial.printf("[IMG] Short read %u/%u\n", got, fsz);
            free(buf); return false;
        }

        // ── JPEG header check ─────────────────────────────────────────────────
        if (got < 4 || buf[0] != 0xFF || buf[1] != 0xD8 || buf[2] != 0xFF) {
            Serial.printf("[IMG] Bad JPEG hdr: %02X%02X%02X\n",
                          buf[0], buf[1], buf[2]);
            free(buf); return false;
        }

        // ── Get JPEG size so we can pick the right scale ───────────────────────
        uint16_t jw = 0, jh = 0;
        TJpgDec.getJpgSize(&jw, &jh, buf, got);
        Serial.printf("[IMG] JPEG %ux%u, buf=%u\n", jw, jh, got);

        if (jw == 0 || jh == 0) {
            Serial.println("[IMG] Could not read JPEG dimensions");
            free(buf); return false;
        }

        int dia = r * 2;

        // TJpgDec supports scale 1/2/4/8 only
        uint8_t scale = 1;
        if      (jw > (uint16_t)dia * 4 || jh > (uint16_t)dia * 4) scale = 8;
        else if (jw > (uint16_t)dia * 2 || jh > (uint16_t)dia * 2) scale = 4;
        else if (jw > (uint16_t)dia     || jh > (uint16_t)dia)     scale = 2;
        TJpgDec.setJpgScale(scale);

        // ── Fill bounding rect with BG before decode ──────────────────────────
        _tft->fillRect(cx - r - 1, cy - r - 1, dia + 2, dia + 2,
                       TFTColors::BG_DARK);

        // ── Point callback at TFT ──────────────────────────────────────────────
        _jpegTft = _tft;
        _jpegX   = (int16_t)(cx - r);
        _jpegY   = (int16_t)(cy - r);

        // drawJpg from memory buffer
        TJpgDec.drawJpg(_jpegX, _jpegY, buf, got);
        free(buf);

        // ── Circle mask: overwrite corners with BG_DARK ────────────────────────
        // Scan only the 4 corner "L-shaped" areas outside the inscribed circle
        // to avoid repainting the entire square.
        int r2 = r * r;
        for (int py = cy - r; py <= cy + r; py++) {
            int dy2 = (py - cy) * (py - cy);
            for (int px = cx - r; px <= cx + r; px++) {
                int dx2 = (px - cx) * (px - cx);
                if (dx2 + dy2 > r2)
                    _tft->drawPixel(px, py, TFTColors::BG_DARK);
            }
        }

        Serial.println("[IMG] Circular JPEG drawn OK");
        return true;
    }
};