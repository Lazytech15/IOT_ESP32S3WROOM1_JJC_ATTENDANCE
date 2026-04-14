// ══════════════════════════════════════════════════════════════════════════════
// employee_profile_display.h  — Portrait 240×320 
// REDESIGN: Full-width square profile, minimalist name, dynamic bottom container.
// ══════════════════════════════════════════════════════════════════════════════
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SD_MMC.h>
#include "TFTDisplayManager.h"
#include "sd_logger.h"
#include <TJpg_Decoder.h>

#ifndef SCREEN_W
#define SCREEN_W 240
#endif
#ifndef SCREEN_H
#define SCREEN_H 320
#endif

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
    String clockType       = "check-in";   
    String profilePicture  = "";           
    bool   accessGranted   = false;
    bool   hasData         = false;
};

// ══════════════════════════════════════════════════════════════════════════════
// TJpgDec output callback
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
        _initTJpgDec();
    }

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

    void showError(const String& message) {
        if (!_tft) return;
        _tft->fillScreen(TFTColors::BG_DARK);
        int cx = SCREEN_W / 2, cy = 110;
        _tft->fillCircle(cx, cy, 40, TFTColors::ERROR);
        _tft->setTextColor(TFTColors::WHITE, TFTColors::ERROR);
        _tft->setTextDatum(MC_DATUM);
        _tft->drawString("X", cx, cy, 6);
        _tft->setTextColor(TFTColors::ERROR, TFTColors::BG_DARK);
        _tft->drawString("ACCESS DENIED", SCREEN_W / 2, 175, 4);
        _tft->setTextColor(TFTColors::TEXT_SECONDARY, TFTColors::BG_DARK);
        String m = message;
        if (m.length() > 26) m = m.substring(0, 26);
        _tft->drawString(m, SCREEN_W / 2, 205, 2);
        _tft->setTextDatum(TL_DATUM);
    }

    void showSuccess(const String& name) {
        // Fast overlay — no full-screen wipe.
        // Stamps a small green "RECORDED" badge over the existing profile card
        // so the transition is near-instant and the employee photo stays visible.
        if (!_tft) return;
        const int BDG_W = 160, BDG_H = 36;
        const int BDG_X = (SCREEN_W - BDG_W) / 2;
        const int BDG_Y = (SCREEN_H / 2) - (BDG_H / 2);
        _tft->fillRoundRect(BDG_X, BDG_Y, BDG_W, BDG_H, 6, TFTColors::SUCCESS);
        _tft->drawRoundRect(BDG_X, BDG_Y, BDG_W, BDG_H, 6, TFTColors::WHITE);
        _tft->setTextColor(TFTColors::WHITE, TFTColors::SUCCESS);
        _tft->setTextDatum(MC_DATUM);
        _tft->drawString("RECORDED", SCREEN_W / 2, BDG_Y + BDG_H / 2, 2);
        _tft->setTextDatum(TL_DATUM);
    }

    void showEmployeeProfile(const EmployeeProfile& emp, const String& photoPath = "") {
        if (!_tft) return;

        SDLogger::logf("EPD", SDLogger::INFO,
            "showEmployeeProfile uid='%s' name='%s' photo='%s'",
            emp.uid.c_str(), emp.fullName.c_str(), photoPath.c_str());

        _tft->fillScreen(TFTColors::BG_DARK);

        // Correct check: clockType is a session key like "morning_in",
        // "afternoon_out", "evening_in" — never the literal "check-in"/"check-out".
        // endsWith("_in") is the only reliable way to detect direction.
        bool isCheckIn = emp.clockType.endsWith("_in");

        // Build the human-readable session label for the action button.
        // e.g. "morning_in"    → "MORNING IN"
        //      "afternoon_out" → "AFTERNOON OUT"
        //      "evening_in"    → "EVENING IN"
        String sessionLabel = emp.clockType.length() > 0 ? emp.clockType : (isCheckIn ? "check-in" : "check-out");
        sessionLabel.replace("_", " ");
        sessionLabel.toUpperCase();

        // Base colors (Green for IN, Red for OUT)
        uint16_t actionCol = isCheckIn ? TFTColors::SUCCESS : TFTColors::RED;

        // ── 1. Full-Width Profile Image ───────────────────────────────────────
        const int IMG_H = 230; // Take up the entire top 230 pixels
        bool photoOk = false;

        if (photoPath.length() > 0) {
            photoOk = _drawRectJpeg(photoPath, 0, 0, SCREEN_W, IMG_H, TFTColors::BG_DARK);
        }

        if (!photoOk) {
            _drawInitialsRect(emp, 0, 0, SCREEN_W, IMG_H, TFTColors::BG_MID, actionCol);
        }

        // Clean up any image spill below our boundary and draw a clean divider line
        _tft->fillRect(0, IMG_H, SCREEN_W, SCREEN_H - IMG_H, TFTColors::BG_DARK);
        _tft->drawFastHLine(0, IMG_H, SCREEN_W, actionCol);
        _tft->drawFastHLine(0, IMG_H + 1, SCREEN_W, actionCol);

        // ── 2. Minimalist Name ────────────────────────────────────────────────
        _tft->setTextDatum(MC_DATUM);
        const int NAME_Y = IMG_H + 20; // Y = 250
        
        _tft->setTextColor(TFTColors::WHITE, TFTColors::BG_DARK);
        String nm = emp.fullName.length() ? emp.fullName : (emp.firstName + " " + emp.lastName);
        // Shrink name if it's too long to fit horizontally
        while (nm.length() > 1 && _tft->textWidth(nm.c_str(), 4) > SCREEN_W - 10)
            nm.remove(nm.length() - 1);
            
        _tft->drawString(nm, SCREEN_W / 2, NAME_Y, 4);

        // ── 3. Dynamic CLOCK IN / OUT Container ───────────────────────────────
        const int BTN_H = 42;
        const int BTN_Y = SCREEN_H - BTN_H - 6; // Y = 272
        const int BTN_W = SCREEN_W - 20;
        const int BTN_X = 10;

        // Fill Base Color
        _tft->fillRoundRect(BTN_X, BTN_Y, BTN_W, BTN_H, 6, actionCol);
        
        // Draw Double Border (White) to make it pop
        _tft->drawRoundRect(BTN_X, BTN_Y, BTN_W, BTN_H, 6, TFTColors::WHITE);
        _tft->drawRoundRect(BTN_X + 1, BTN_Y + 1, BTN_W - 2, BTN_H - 2, 5, TFTColors::WHITE);
        
        // Show session label: "MORNING IN", "AFTERNOON OUT", "EVENING IN", etc.
        // Scale font down slightly if the label is too wide for font 4.
        _tft->setTextColor(TFTColors::WHITE, actionCol);
        uint8_t btnFont = (_tft->textWidth(sessionLabel.c_str(), 4) <= BTN_W - 8) ? 4 : 2;
        _tft->drawString(sessionLabel, SCREEN_W / 2, BTN_Y + (BTN_H / 2), btnFont);
        
        _tft->setTextDatum(TL_DATUM);

        SDLogger::logf("EPD", SDLogger::INFO, "Profile drawn successfully");
    }

private:
    TFT_eSPI* _tft = nullptr;

    void _initTJpgDec() {
        TJpgDec.setJpgScale(1);
        TJpgDec.setCallback(_jpegCB);
        TJpgDec.setSwapBytes(true); // REQUIRED: Fixes endianness for SPI
    }

    void _verifyTJpgDecInit() {
        TJpgDec.setCallback(_jpegCB);
        TJpgDec.setSwapBytes(true); // REQUIRED
    }

    // Fallback: Draws huge initials if there is no photo
    void _drawInitialsRect(const EmployeeProfile& emp, int x, int y, int w, int h, uint16_t bg, uint16_t fg) {
        _tft->fillRect(x, y, w, h, bg);
        String ini;
        if (emp.firstName.length())  ini += (char)toupper(emp.firstName[0]);
        if (emp.lastName.length())   ini += (char)toupper(emp.lastName[0]);
        if (!ini.length() && emp.fullName.length())
            ini = String((char)toupper(emp.fullName[0]));
        
        _tft->setTextColor(fg, bg);
        _tft->setTextDatum(MC_DATUM);
        
        // Temporarily scale up font 4 to make it massive
        _tft->setTextSize(3);
        _tft->drawString(ini, x + w / 2, y + h / 2, 4);
        _tft->setTextSize(1); // Reset!
        
        _tft->setTextDatum(TL_DATUM);
    }

    // Core Drawing Engine for Rectangular Constraints
    bool _drawRectJpeg(const String& path, int x, int y, int maxW, int maxH, uint16_t bg) {
        if (!_tft || !SD_MMC.exists(path)) return false;

        File f = SD_MMC.open(path, FILE_READ);
        if (!f || f.size() < 4) { if (f) f.close(); return false; }
        uint8_t magic[4] = {0};
        f.read(magic, 4);
        f.close();

        if (magic[0] != 0xFF || magic[1] != 0xD8 || magic[2] != 0xFF) {
            SD_MMC.remove(path);
            return false;
        }

        _verifyTJpgDecInit();
        TJpgDec.setJpgScale(1);

        uint16_t jw = 0, jh = 0;
        const size_t PEEK = 512;
        uint8_t peek[PEEK];
        File fp = SD_MMC.open(path, FILE_READ);
        size_t peeked = fp ? fp.read(peek, PEEK) : 0;
        if (fp) fp.close();

        if (peeked >= 4) TJpgDec.getJpgSize(&jw, &jh, peek, peeked);

        if ((jw == 0 || jh == 0) && SD_MMC.exists(path)) {
            const size_t PEEK2 = 2048;
            uint8_t* peek2 = (uint8_t*)malloc(PEEK2);
            if (peek2) {
                File fp2 = SD_MMC.open(path, FILE_READ);
                size_t p2 = fp2 ? fp2.read(peek2, PEEK2) : 0;
                if (fp2) fp2.close();
                if (p2 >= 4) TJpgDec.getJpgSize(&jw, &jh, peek2, p2);
                free(peek2);
            }
        }

        // Scale determination based on boundary limits
        uint8_t scale = 1;
        if      (jw >= maxW * 8 || jh >= maxH * 8) scale = 8;
        else if (jw >= maxW * 4 || jh >= maxH * 4) scale = 4;
        else if (jw >= maxW * 2 || jh >= maxH * 2) scale = 2;
        if (jw == 0 || jh == 0) scale = 1; 

        TJpgDec.setJpgScale(scale);

        int rw = (jw > 0) ? (int)(jw / scale) : maxW;
        int rh = (jh > 0) ? (int)(jh / scale) : maxH;
        
        // This forces the JPEG Decoder to center the image perfectly.
        // If the image is taller or wider than maxW/maxH, the coordinates become negative,
        // which forces the library to natively 'crop' it like CSS object-fit: cover!
        int16_t drawX = x + (maxW - rw) / 2;
        int16_t drawY = y + (maxH - rh) / 2;

        _tft->fillRect(x, y, maxW, maxH, bg);

        _verifyTJpgDecInit();
        _jpegTft = _tft;
        _jpegX   = 0;
        _jpegY   = 0;

        JRESULT res = TJpgDec.drawFsJpg(drawX, drawY, path.c_str(), SD_MMC);

        if (res != JDR_OK) {
            if (res == JDR_FMT1 || res == JDR_FMT2 || res == JDR_FMT3) SD_MMC.remove(path);
            return false;
        }

        return true;
    }
};