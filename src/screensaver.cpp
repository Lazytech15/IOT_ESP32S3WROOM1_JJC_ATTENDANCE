// ══════════════════════════════════════════════════════════════════════════════
// screensaver.cpp
// ══════════════════════════════════════════════════════════════════════════════
#include "screensaver.h"
#include <SD_MMC.h>
#include <TJpg_Decoder.h>
#include "TFTDisplayManager.h"
#include "sd_mutex.h"

// ── Module state ────────────────────────────────────────────────────────────
static TFT_eSPI* _ssTft          = nullptr;
static bool      _logoAvailable  = false;
static uint16_t  _logoW          = 0;
static uint16_t  _logoH          = 0;
static int16_t   _logoDrawX      = 0;   // top-left x/y the logo is centered at
static int16_t   _logoDrawY      = SS_LOGO_Y;

// TJpg_Decoder output callback. Named distinctly from EmployeeProfileDisplay's
// _jpegCB — TJpg_Decoder is a shared singleton so whoever draws last must
// re-arm its own callback right before drawing (see ssVerifyTJpgDecInit()).
//
// IMPORTANT: TJpg_Decoder::drawFsJpg(x, y, ...) already stores the x/y you
// pass it (jpeg_x/jpeg_y) and adds it to every block's coordinates *inside
// the library* before calling this callback (see TJpg_Decoder::jd_output()).
// So the x/y args received here are already absolute screen coordinates —
// adding _logoDrawX/_logoDrawY again double-offsets the logo (this was the
// bug: it pushed the logo ~55px right and ~20px down from where it should
// be, which is why it looked shifted off-center instead of top-centered).
static bool ssJpegCB(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
    if (!_ssTft) return false;
    _ssTft->pushImage(x, y, w, h, bmp);
    return true;
}

static void ssVerifyTJpgDecInit() {
    TJpgDec.setCallback(ssJpegCB);
    TJpgDec.setSwapBytes(true);   // required for correct colors over SPI
}

// Reads just enough of the JPEG header to get its pixel dimensions, using
// the same peek-512-then-peek-2048 fallback as EmployeeProfileDisplay.
static void ssReadJpgSize(const char* path, uint16_t& jw, uint16_t& jh) {
    jw = jh = 0;

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
}

// ── Init ─────────────────────────────────────────────────────────────────────
void screensaverInit() {
    _ssTft = TFTDisplayManager::getTFT();

    if (!SD_MMC.exists(SCREENSAVER_LOGO_PATH)) {
        Serial.println("[Screensaver] /logo.jpg not found on SD card — "
                        "copy your logo there. Screensaver will show clock/date only.");
        _logoAvailable = false;
        return;
    }

    // Sanity-check the JPEG magic bytes, same guard EmployeeProfileDisplay uses.
    File f = SD_MMC.open(SCREENSAVER_LOGO_PATH, FILE_READ);
    uint8_t magic[4] = {0};
    if (f) { f.read(magic, 4); f.close(); }
    if (magic[0] != 0xFF || magic[1] != 0xD8 || magic[2] != 0xFF) {
        Serial.println("[Screensaver] /logo.jpg is not a valid JPEG — skipping logo");
        _logoAvailable = false;
        return;
    }

    ssVerifyTJpgDecInit();
    TJpgDec.setJpgScale(1);
    ssReadJpgSize(SCREENSAVER_LOGO_PATH, _logoW, _logoH);

    if (_logoW == 0 || _logoH == 0) {
        Serial.println("[Screensaver] WARNING: could not read /logo.jpg dimensions — "
                        "logo will be skipped");
        _logoAvailable = false;
        return;
    }

    if (_logoW > SS_LOGO_MAX_W || _logoH > SS_LOGO_MAX_H) {
        Serial.printf("[Screensaver] WARNING: /logo.jpg is %ux%u, larger than the "
                      "%ux%u budget on screen — resize it on the SD card for best "
                      "results (it will still draw, just may overflow the box)\n",
                      _logoW, _logoH, SS_LOGO_MAX_W, SS_LOGO_MAX_H);
    }

    _logoAvailable = true;
    Serial.printf("[Screensaver] Logo ready: %ux%u\n", _logoW, _logoH);
}

bool screensaverLogoAvailable() { return _logoAvailable; }

// ── Full redraw ──────────────────────────────────────────────────────────────
void drawScreensaver() {
    if (!_ssTft) return;

    _ssTft->fillScreen(TFT_BLACK);

    if (_logoAvailable) {
        ssVerifyTJpgDecInit();

        // Pick a decode scale (1/2/4/8) so the logo always fits inside the
        // SS_LOGO_MAX_W x SS_LOGO_MAX_H budget, same approach
        // EmployeeProfileDisplay uses for photos. Centering is computed from
        // the actual *scaled* size (and the TFT's runtime width, not a
        // hardcoded macro) so this stays centered regardless of what
        // resolution image gets uploaded through the portal.
        uint8_t scale = 1;
        if      (_logoW >= SS_LOGO_MAX_W * 8 || _logoH >= SS_LOGO_MAX_H * 8) scale = 8;
        else if (_logoW >= SS_LOGO_MAX_W * 4 || _logoH >= SS_LOGO_MAX_H * 4) scale = 4;
        else if (_logoW >= SS_LOGO_MAX_W * 2 || _logoH >= SS_LOGO_MAX_H * 2) scale = 2;
        TJpgDec.setJpgScale(scale);

        int16_t scaledW = (int16_t)(_logoW / scale);
        int16_t scaledH = (int16_t)(_logoH / scale);
        int16_t screenW = _ssTft->width();   // runtime, not the SCREEN_W macro

        _logoDrawX = (screenW - scaledW) / 2;
        _logoDrawY = SS_LOGO_Y;

        JRESULT res;
        {
            SDLockGuard _sdLock;   // serialize SD_MMC access across tasks — see sd_mutex.h
            res = TJpgDec.drawFsJpg(_logoDrawX, _logoDrawY,
                                     SCREENSAVER_LOGO_PATH, SD_MMC);
        }
        if (res != JDR_OK) {
            Serial.printf("[Screensaver] drawFsJpg failed (code %d)\n", (int)res);
        }

        TJpgDec.setJpgScale(1);   // reset — other modules (employee photos) expect scale 1
    }

    // The very next clock tick in loop() calls updateScreensaverClock()/
    // updateScreensaverDate() with the real values, so we don't need to
    // paint placeholder text here.
}

// ── Per-second repaint (no flicker: same font/position, background fills) ──
void updateScreensaverClock(uint8_t h, uint8_t m, uint8_t s) {
    if (!_ssTft) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);

    int16_t screenW = _ssTft->width();
    _ssTft->setTextDatum(MC_DATUM);
    _ssTft->setTextColor(TFT_WHITE, TFT_BLACK);
    _ssTft->drawString(buf, screenW / 2, SS_TIME_Y, 7);   // font 7 = 7-seg digits
}

void updateScreensaverDate(const String& dateStr) {
    if (!_ssTft) return;

    int16_t screenW = _ssTft->width();

    // Fixed-width blank first to fully erase a longer previous string
    // (month names vary in length: "August" vs "May").
    _ssTft->fillRect(0, SS_DATE_Y - 13, screenW, 26, TFT_BLACK);

    _ssTft->setTextDatum(MC_DATUM);
    _ssTft->setTextColor(TFT_WHITE, TFT_BLACK);
    _ssTft->drawString(dateStr, screenW / 2, SS_DATE_Y, 4);
}
