# JJC NFC Attendance System

An ESP32-S3-based NFC attendance terminal that records employee time-in/time-out using NFC cards or tags. The device operates fully offline using an SD card and syncs records to a remote server whenever a network connection is available.

---

## Screenshot

<img width="2983" height="2559" alt="ncfattendance" src="https://github.com/user-attachments/assets/cb19aa3f-6e57-4d22-985e-b84efac86665" />


## Features

- **NFC Card/Tag Scanning** — Employees tap their NFC card or tag to clock in or out. The PN532 module reads ISO14443A cards with a built-in noise filter, ghost-card debounce, and cooldown gate to prevent duplicate scans.
- **Session-Aware Clock Types** — Automatically determines whether a tap is a `morning_in`, `morning_out`, `afternoon_in`, `afternoon_out`, `evening_in`, or `evening_out` by reading today's existing records from the SD card.
- **AES-256-CBC Encrypted Server Responses** — All API responses from the server are encrypted. The device decrypts them using mbedTLS with a shared base64-encoded key. A two-pass fallback also handles plain JSON responses gracefully.
- **Offline-First / SD-First Architecture** — The device boots and operates entirely from SD card data. Internet connectivity is optional and used only to sync employee profiles and upload attendance records.
- **Fast-Tap Upload Queue** — Every scan is written to the SD card immediately (< 5 ms). A background scheduler uploads queued records to the server every 3 seconds without blocking the NFC scanner.
- **TFT Display** — A 240×320 ST7789 display shows a live dashboard (clock, date, check-in/check-out counters, last scan) and an employee profile card with photo on each tap.
- **Web Portal** — A built-in HTTP server (port 8080) serves a local web dashboard accessible from any browser on the same network.
- **Real-Time Event Polling** — Polls `/api/socket?action=poll` every 8 seconds to receive `attendance_created` and `attendance_updated` events from other devices or the web portal.
- **Periodic Re-Seed** — Re-fetches today's attendance from the server every 5 minutes so the local CSV stays in sync with records entered elsewhere.
- **Employee Photo Cache** — Profile photos are downloaded once and cached on the SD card for offline display on subsequent taps.
- **NTP Time Sync** — Syncs time from `pool.ntp.org` on boot and every hour. Timezone is set to UTC+8 (Philippine Standard Time).
- **Wi-Fi Captive Portal** — If no saved Wi-Fi credentials are found, the device broadcasts an AP (`JJC_Attendance_Config`) so credentials can be configured via browser.

---

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32-S3 WROOM-1 (N8 or N8R8) |
| NFC Module | Adafruit PN532 (SPI mode) |
| Display | ST7789 2.8" TFT (240×320, SPI) |
| Storage | MicroSD card (FAT32) |
| Flash | 8 MB (huge_app partition) |

### Pin Assignments

#### TFT Display (ST7789)

| Signal | GPIO |
|---|---|
| MOSI | 4 |
| SCK | 5 |
| CS | 6 |
| DC | 7 |
| RST | 8 |
| Backlight | 9 |

#### NFC Module (PN532)

| Signal | GPIO |
|---|---|
| SS | 10 |
| MOSI | 11 |
| SCK | 12 |
| MISO | 13 |

#### SD Card

| Signal | GPIO |
|---|---|
| MOSI | 39 |
| SCK | 40 |
| MISO | 41 |
| CS | 42 |

> **Note:** If your board is the N8R8 (PSRAM) variant, GPIOs 33–37 are reserved for PSRAM. Use the GPIO 39–42 assignments above. Avoid strapping pins: GPIO 0, 3, 45, 46.

---

## Software Dependencies

Managed via PlatformIO. Declared in `platformio.ini`:

| Library | Version |
|---|---|
| bodmer/TFT_eSPI | ^2.5.43 |
| adafruit/Adafruit PN532 | ^1.3.1 |
| adafruit/Adafruit BusIO | latest |
| bblanchon/ArduinoJson | ^6.21.3 |
| bodmer/TJpg_Decoder | ^1.0.8 |

mbedTLS (AES, Base64) is included with the ESP32 Arduino framework — no extra library needed.

---

## Project Structure

```
Final_New_Attendance_Record/
├── platformio.ini                  # Build configuration, pin flags, library deps
└── src/
    ├── main.cpp                    # Boot sequence, loop, NFC scan pipeline, upload queue
    ├── aes_decryptor.h             # AES-256-CBC decryption + two-pass server response handler
    ├── pin_assignments.h           # All GPIO definitions
    ├── attendance_http_service.h   # HTTP client for all server API calls
    ├── attendance_display_profile.h
    ├── dashboard.cpp / .h          # TFT dashboard UI (clock, stats, last scan strip)
    ├── employee_profile_display.h  # Profile card + loading/error/success screens
    ├── employee_sync.h             # Full and incremental employee sync from server
    ├── nfc_manager.cpp / .h        # PN532 init, card read, UID extraction
    ├── offline_sync.h              # Offline queue helpers
    ├── sd_database.cpp / .h        # SD card: employee profiles, attendance CSV, photos
    ├── sd_file_manager.h           # Low-level SD file operations
    ├── sd_logger.h                 # Serial + SD panic logger
    ├── TFTDisplayManager.cpp / .h  # TFT_eSPI wrapper and initialization
    ├── WiFiConfig.cpp / .h         # Wi-Fi connection + captive portal AP
    └── WiFiManager.h               # Built-in HTTP server + SSE event broadcaster
```

---

## How It Works

### Boot Sequence

1. **TFT** initialises and shows a loading progress bar.
2. **SD card** mounts. If successful, offline mode is available immediately.
3. **NFC** (PN532) initialises over software SPI.
4. **Wi-Fi** connects using saved credentials or starts the config AP.
5. **NTP** time sync (UTC+8). The software clock ticks from this point.
6. **Web portal** starts on port 8080.
7. **Initial sync** — employee profiles are pulled from the server (incremental if a local cache exists), and today's attendance is seeded into the local CSV so clock-type resolution is correct from the first tap.

### NFC Scan Pipeline

```
Card Tap
  │
  ├─ Noise filter (< 8 hex chars → reject)
  ├─ Confirm gate (2 consecutive matching reads required)
  ├─ Cooldown gate (3.5 s same-card lockout)
  │
  ▼
handleNFCDetected()
  │
  ├─ 1. Resolve employee UID from card ID (SD mapping file)
  ├─ 2. Load employee profile from SD  ◄── offline-capable
  ├─ 3. Fetch from server if SD miss   ◄── requires Wi-Fi; caches result to SD
  ├─ 4. Ensure profile photo cached on SD
  ├─ 5. Determine clock type (morning_in/out, afternoon_in/out, evening_in/out)
  ├─ 6. Show employee profile card on TFT
  ├─ 7. Log attendance to SD immediately  ◄── < 5 ms, guaranteed local record
  └─ 8. Enqueue for background upload     ◄── non-blocking, returns immediately
```

### Offline → Online Sync

When Wi-Fi is unavailable, every scan is written to the SD card CSV. When connectivity is restored, the background uploader (`flushPending`) drains the queue at a 3-second interval, sending up to 3 records per cycle to avoid watchdog timeouts.

### Encrypted Server Communication

The server wraps API responses in an AES-256-CBC encrypted envelope:

```json
{ "encrypted": true, "data": "<base64(IV || ciphertext)>" }
```

The device decodes the base64 payload, splits the first 16 bytes as the IV, decrypts the remainder using the shared key, strips PKCS#7 padding, and parses the resulting JSON. If the `encrypted` flag is absent (e.g. the middleware was bypassed), the raw body is parsed directly as a fallback.

---

## Configuration

All run-time constants are defined at the top of `main.cpp`:

| Constant | Default | Description |
|---|---|---|
| `SERVER_URL` | `https://jjcenggworks.com` | Base URL of the attendance server |
| `AP_SSID` | `JJC_Attendance_Config` | Wi-Fi AP name for config portal |
| `AP_PASSWORD` | `ilovejjcenggworks` | Wi-Fi AP password |
| `NFC_POLL_INTERVAL_MS` | 150 | PN532 poll cadence (ms) |
| `SCAN_COOLDOWN_MS` | 3500 | Same-card lockout after accepted scan |
| `CARD_CONFIRM_NEEDED` | 2 | Consecutive matching reads before accepting |
| `UPLOAD_FLUSH_MS` | 3000 | Background upload interval (ms) |
| `UPLOAD_BATCH_SIZE` | 3 | Max records per upload cycle |
| `SOCKET_POLL_MS` | 8000 | Real-time event polling interval (ms) |
| `RESEED_INTERVAL_MS` | 300000 | Periodic SD re-seed from server (5 min) |
| `PROFILE_DISPLAY_MS` | 2000 | Profile card display duration (ms) |

The AES-256 encryption key is a base64-encoded 32-byte value shared with the server. Set it in `attendance_http_service.h` to match the server's `API_ENCRYPTION_KEY`.

---

## Building & Flashing

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Open the `Final_New_Attendance_Record` folder as a PlatformIO project.
3. Connect the ESP32-S3 via USB-C.
4. Build and upload:

```bash
pio run --target upload
```

5. Open the serial monitor at 115200 baud:

```bash
pio device monitor
```

> The ESP32-S3 uses USB-CDC. The monitor asserts DTR/RTS automatically (`monitor_dtr = 1`, `monitor_rts = 1` in `platformio.ini`) so `Serial` output appears immediately after the port is opened.

---

## SD Card Layout

```
/
├── employees/
│   ├── <uid>.json          # Employee profile (name, dept, NFC mapping, etc.)
│   ├── sync_meta.json      # Tracks last sync timestamp for incremental updates
│   └── nfc_map.csv         # Maps raw card IDs to employee UIDs
├── photos/
│   └── <uid>.jpg           # Cached profile photos
└── attendance/
    └── YYYY-MM-DD.csv      # Daily attendance log (one row per clock event)
```

---

## Server API Endpoints Used

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/attendance/nfc-auth` | Authenticate an NFC card and fetch employee profile |
| POST | `/api/attendance/record` | Submit an attendance record |
| GET | `/api/attendance?date=` | Fetch raw attendance rows for today |
| GET | `/api/attendance/esp32-sync?date=` | Fetch daily summary snapshot (fast path) |
| GET | `/api/employees` | Full or incremental employee list sync |
| GET | `/api/socket?action=poll&since=` | Long-poll for real-time events |

All responses are AES-256-CBC encrypted by the server's encryption middleware. The device handles both encrypted and plain-JSON responses transparently.

---

## License

This project is proprietary software developed for JJC Engineering Works. All rights reserved.
