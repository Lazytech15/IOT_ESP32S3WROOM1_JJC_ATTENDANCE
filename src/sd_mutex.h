// ══════════════════════════════════════════════════════════════════════════════
// sd_mutex.h
//
// WHY THIS EXISTS:
//   nfcWorkerTask, uploadWorkerTask, and seedWorkerTask are three separate
//   FreeRTOS tasks, all pinned to Core 0, and all call SD_MMC.open()/read()/
//   write() directly. There was no lock between them. g_networkMutex (in
//   main.cpp) only serializes the *HTTP* calls made by upload vs seed — it
//   was deliberately NOT used to gate NFC's own SD/HTTP work, so a tap would
//   never wait behind a multi-minute seed pass.
//
//   The gap: a seed pass (seedTodayAttendanceFromServer(), see
//   attendance_http_service.h) holds one or more SD_MMC File handles open
//   for the ENTIRE pass while it streams a snapshot file to disk — and the
//   idle-reconcile burst in main.cpp can trigger this pass repeatedly during
//   any long idle stretch (screen off, 08:00-17:00). The longer the device
//   sits idle, the more of these multi-minute SD-writing passes run, and the
//   higher the odds an employee's tap lands mid-write and collides with the
//   seed pass on the same SD_MMC filesystem from a different task — SD_MMC/
//   FATFS on this core isn't safe for that. Symptom: the tap silently does
//   nothing, or the SD subsystem wedges outright and needs a reboot to
//   recover. This is what's meant here by "lag on long idle".
//
// FIX:
//   One recursive mutex, held only around actual SD_MMC/File calls (never
//   around network calls or long-running loops) so:
//     • nfcWorkerTask (priority 2) only ever waits a few ms for a single
//       file op to finish, never for a whole multi-minute seed pass.
//     • seedWorkerTask/uploadWorkerTask take/release it PER FILE OPERATION
//       (see attendance_http_service.h's snapshot loop and offline_sync.h),
//       not once for the whole pass — so a waiting tap gets serviced between
//       pages instead of being blocked for minutes.
//   Recursive because SDDatabase methods sometimes call one another from the
//   same task (e.g. logAttendance() -> ensureDir()/todayFilename()); a plain
//   mutex would self-deadlock on that.
// ══════════════════════════════════════════════════════════════════════════════

#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t g_sdMutex;

// Call once from setup(), before any task that touches the SD card starts.
inline void sdMutexInit() {
    if (!g_sdMutex) g_sdMutex = xSemaphoreCreateRecursiveMutex();
}

// RAII guard — take the lock for the lifetime of the scope, release on
// every return path (including early `return`/`break` inside the guarded
// function) via the destructor.
class SDLockGuard {
public:
    SDLockGuard()  { if (g_sdMutex) xSemaphoreTakeRecursive(g_sdMutex, portMAX_DELAY); }
    ~SDLockGuard() { if (g_sdMutex) xSemaphoreGiveRecursive(g_sdMutex); }
    SDLockGuard(const SDLockGuard&) = delete;
    SDLockGuard& operator=(const SDLockGuard&) = delete;
};
