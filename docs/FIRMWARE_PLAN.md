# AirTrack firmware plan

Status: 1.1.0 is built and flashed on the connected Waveshare
ESP32-C6-LCD-1.47 (non-touch, C6FH8 hardware revision). Since 1.0.0 the ADS-B
client reuses its TLS connection, the state machine returns from recovery
setup to tracking on its own, the LCD and LAN dashboard were redesigned, and
the setup portal exposes the remaining tracker/display settings. Phases 0-4
and the core of Phases 5-6 are implemented: board support, provisioning,
versioned settings, verified-HTTPS ADS-B polling, bounded parsing/ranking,
production LCD states, the LAN dashboard/API, and optional SD event logging.

The connected unit has passed the reproducible release gate and a hardware
boot/LAN smoke test. It mounted the 28.9 GiB FAT32 card without formatting,
preserved its saved WPA3 network, obtained DHCP address `10.0.0.104`, started
mDNS and the dashboard, synchronized time, and remained stable across repeated
status requests. Final production sign-off still requires a real configured
location for on-target adsb.fi/TLS validation and the planned long-duration
soak; coordinates are deliberately never guessed by the firmware or build
process. Signed OTA/rollback and advanced log retention/downloads remain
deferred.

Visual references: [revised device states](ui/device-states-concept-v2.png) and [web configurator](ui/web-configurator-concept.png).

## 1. Product boundary

AirTrack is a fixed-location, Wi-Fi-connected enthusiast display. It queries adsb.fi, selects the closest fresh aircraft within a configured radius, and presents that aircraft on the 172 x 320 LCD and local web dashboard.

The device is not a receiver, collision-warning instrument, or navigation aid. It has no onboard GPS or battery management. Location is configured through the local web interface and power is supplied over USB-C.

## 2. Locked technical decisions

- Framework: native ESP-IDF 5.5.5, pinned for reproducible builds.
- Language: C for drivers and application components; no Arduino compatibility layer.
- UI: LVGL 9.5 with Espressif's LVGL port 2.8 and two 172 x 20 RGB565 DMA strip buffers.
- Initial managed-component pins: `lvgl/lvgl` 9.5.0, `espressif/esp_lvgl_port` 2.8.0, `espressif/qrcode` 0.2.0, `espressif/led_strip` 3.0.3, and `espressif/mdns` 1.11.3; commit the generated dependency lock.
- Display orientation: portrait, 172 x 320, RGB565, BGR order, 34-pixel X RAM offset.
- Networking: 2.4 GHz Wi-Fi station mode, with AP+STA recovery provisioning when required.
- Provisioning: per-device WPA2 hotspot, scannable Wi-Fi QR, captive portal, and manual SSID/password/IP fallback.
- Internet transport: verified HTTPS using the ESP x509 certificate bundle. Insecure TLS is not permitted.
- Data handling: bounded streaming parsing; the complete adsb.fi response is never retained in RAM.
- Settings: checksummed, versioned NVS blob. Wi-Fi secrets are never returned by the API or written to SD.
- SD: optional FAT32 logging only. The device remains fully operational without a card and never auto-formats one.
- Updates: dual OTA app slots are reserved from the beginning; web upload/remote OTA is a later milestone.

## 3. Hardware source of truth

| Function | Setting |
|---|---|
| MCU | Connected unit: ESP32-C6FH8 rev 0.2, 160 MHz, 8 MB embedded flash, no external PSRAM |
| LCD | ST7789-family 1.47-inch TFT, 172 x 320, SPI mode 0 |
| LCD SPI | MOSI GPIO6, SCLK GPIO7, CS GPIO14, DC GPIO15 |
| LCD control | Reset GPIO21, active-high backlight PWM GPIO22 |
| LCD geometry | 34-pixel X offset, 0-pixel Y offset, portrait mirror-X orientation |
| Vendor-safe LCD clock | 12 MHz initially; increase only after hardware testing |
| SD SPI | CS GPIO4, MISO GPIO5, MOSI GPIO6, SCLK GPIO7 |
| RGB LED | One WS2812B on GPIO8 |
| BOOT input | GPIO9, active low; holding during reset enters ROM download mode |
| Native USB | D- GPIO12, D+ GPIO13; Serial/JTAG console at 115200 baud |
| Exposed UART0 | TX GPIO16, RX GPIO17 |

The SD card and LCD share SPI2. The board component must initialize the bus exactly once, place the SD card into SPI mode before adding the LCD device, and serialize SD activity with asynchronous LCD flushes. Configure `max_transfer_sz` to at least 6,880 bytes for a complete 20-line RGB565 strip; use 8 KiB initially.

Production SPI initialization order is fixed:

1. Keep the backlight off, drive LCD CS and SD CS high, and preferably hold LCD reset asserted.
2. Initialize SPI2 once with GPIO6/GPIO5/GPIO7 and automatic DMA selection.
3. Attempt the non-formatting SD mount so the card enters SPI mode before any LCD traffic.
4. Attach the LCD device to the existing bus, run the vendor panel sequence, draw the first frame, then enable the backlight.

Start the SD clock at 10 MHz and the LCD at Waveshare's 12 MHz. Raise the SD clock only after the combined bus passes soak testing. Use an LCD transaction queue depth of 2-4 rather than the demo's 10.

Place a priority-inheritance `spi_gate` around logical display and storage operations. The UI task takes the gate, queues one flush, waits for a task notification from the LCD completion callback, marks the LVGL flush ready, and releases the gate from the UI task. The ISR callback only notifies; it never releases a FreeRTOS mutex. The storage task takes the same gate for one bounded append or metadata operation.

GPIO8 is also a boot-strapping pin, so the RGB/RMT driver is initialized only after normal startup. GPIO9 is treated as a runtime button only after the boot sequence has completed.

The official demo is a bring-up reference, not an application base. Do not inherit its automatic SD formatting, BLE scanner, PSRAM settings, demo tasks, 4,000-byte SPI transfer limit, temporary 75% backlight setting, or full-screen transfer budget. Port and verify the panel initialization table with its original license notice. In particular, A/B test the vendor `D0` command on real hardware: the native source builds `{0xA4, 0xA1}` but transmits only the first byte, whereas the Arduino variant transmits both.

## 4. Repository layout

```text
/
|-- CMakeLists.txt
|-- sdkconfig.defaults
|-- partitions.csv
|-- dependencies.lock
|-- main/
|   `-- app_main.c
|-- components/
|   |-- app/             # supervisor and top-level state reduction
|   |-- model/           # fixed-size config, aircraft, snapshot, and events
|   |-- board/           # pins, SPI2, ST7789, SD mount, backlight, LED, BOOT
|   |-- config/          # NVS persistence, validation, migration, redaction
|   |-- connectivity/    # STA/AP, scanning, DHCP options, SNTP, mDNS
|   |-- captive_dns/     # bounded wildcard DNS responder for setup mode
|   |-- adsb_client/     # HTTPS request and bounded response stream parser
|   |-- tracker/         # filtering, ranking, fallback geometry, hysteresis
|   |-- display/         # LVGL screens and network footer
|   |-- web/             # HTTP server, REST/jobs, embedded gzip assets
|   |-- storage/         # optional buffered/rotated SD event log
|   `-- diagnostics/     # health counters, heap/stack metrics, reset cause
|-- web/
|   |-- src/             # dependency-free HTML, CSS, and JavaScript
|   `-- dist/            # reproducibly generated gzip assets
|-- test/
|   |-- fixtures/        # captured and synthetic adsb.fi responses
|   `-- host/            # parser, tracker, config, and QR tests
|-- tools/               # web packing, fixture validation, size checks
`-- docs/
```

Dependencies flow inward through `model`; UI, web, and storage consume snapshots and never call the ADS-B client directly.

## 5. Runtime architecture

```text
Wi-Fi/IP events -----> supervisor <------ config changes
       |                   |
       |                   +----> display command queue ----> LVGL task
       |                   |
SNTP --+--> ADS-B worker --+----> current snapshot ---------> web handlers
                   |       |
                   |       `----> bounded log queue --------> storage task
                   `--> HTTPS stream parser --> tracker
```

### Task ownership

| Context | Suggested stack/priority | Ownership |
|---|---:|---|
| Supervisor | 5 KiB / 5 | State transitions, timers, config-change coordination |
| LVGL port task | 6 KiB / 4 | All LVGL object creation and mutation |
| ADS-B worker | 10 KiB / 3 | HTTPS, cooperative streaming parse, candidate construction |
| Storage task | 4 KiB / 2 | Batched SD writes, rotation, retention |
| HTTP server task | 7 KiB / 4 | Static files and bounded API handlers |
| Captive DNS task | 3 KiB / 1 | Active only while the setup AP is running |

ESP32-C6 is single-core, so tasks should not be pinned. Wi-Fi callbacks only publish events; they never perform DNS, HTTP, NVS, filesystem, or UI work.

The latest `AircraftSnapshot` is a fixed-size immutable value. The ADS-B worker publishes it through a one-element overwrite queue or double buffer. Consumers copy it; no consumer retains parser-owned pointers.

## 6. Boot and connection state machine

```text
BOOT
  -> initialize NVS/config
  -> initialize LED and BOOT input
  -> initialize SPI2
  -> attempt optional SD mount
  -> initialize LCD, draw first frame, then enable backlight
  -> no valid Wi-Fi credentials? SETUP_AP
  -> otherwise STA_CONNECTING

STA_CONNECTING
  -> got IPv4: TIME_SYNC
  -> no connection for 60 s: RECOVERY_AP_STA

TIME_SYNC
  -> SNTP valid: ONLINE
  -> timeout: keep retrying and show TIME SYNC; do not bypass TLS checks

ONLINE
  -> poll adsb.fi
  -> Wi-Fi lost: RECONNECTING
  -> repeated failure for 60 s: RECOVERY_AP_STA

RECOVERY_AP_STA
  -> show QR/setup credentials while retrying STA every 20 s
  -> configured STA holds IPv4 for 5 s: stop AP/DNS/portal and return ONLINE
  -> BOOT held 5 s: restart
```

Additional transitions:

- Holding BOOT for 5 seconds at runtime forces setup mode without erasing settings.
- Loss of Internet or an adsb.fi error does not start the setup AP if local Wi-Fi still has an IP. It produces an API-offline/stale display state instead.
- Setup mode pauses ADS-B polling and SD logging to maximize memory and keep the portal responsive.
- A network footer becomes visible as soon as `IP_EVENT_STA_GOT_IP` is received and remains on every connected display state.
- Candidate credentials are committed only after the station holds a valid IPv4 address continuously for 30 seconds. Internet/API availability is reported separately and does not invalidate working LAN credentials.
- After a successful provisioning job, keep the setup AP alive for a 10-second confirmation grace period, then stop AP/DNS to recover RAM and airtime.

## 7. QR provisioning and captive portal

The hotspot identity is generated once and retained across ordinary reboots:

- SSID: `AirTrack-` plus the final four hexadecimal MAC characters.
- Password: a random, unambiguous 8-character base32 secret stored in NVS
  (the minimum length accepted by WPA2-Personal).
- Address: `192.168.4.1`.
- QR payload: `WIFI:T:WPA;S:<escaped-ssid>;P:<escaped-password>;H:false;;`.

Use Espressif's QR component to generate the matrix at runtime. Keep the payload within QR version 4-M where practical: its 33-module matrix plus a four-module quiet zone on each side is 41 modules, which fits at four pixels per module as a 164 x 164 monochrome symbol. Render modules directly as black/white rectangles rather than allocating a full RGB bitmap. Test it with multiple Android and iOS cameras. The SSID, password, and `192.168.4.1` remain visible below the QR for manual connection.

The setup AP runs DHCP plus a fixed-buffer DNS responder that maps valid A and
ANY questions to `192.168.4.1`. DHCP advertises that address as DNS and sends
`http://192.168.4.1/` in Captive-Portal Option 114. HTTP handlers cover common
Android, Apple, and Windows connectivity probes, while a final wildcard GET
handler catches other probe paths and sends them to the numeric AP origin.

Wildcard requests are redirected to the numeric AP origin before the form is
served. The form posts only to that canonical origin, requires a random
128-bit per-start token, rejects noncanonical `Host` values, and disables
credential autocomplete. This keeps captive-probe hostnames from becoming
alternate writable configuration origins.

Before the setup hotspot begins beaconing, the station radio performs one
bounded preflight scan. This prevents remembered clients from probing a live
AP before its HTTP and DNS services are ready. Results are deduplicated by
SSID, retain the strongest access point, sort by descending RSSI, and cap both
raw allocation and rendered output. The form shows at most 16 networks with
signal and open/locked status, while keeping an editable SSID field for hidden
networks.

When the station interface is connected, the persistent bottom area is reserved for:

```text
<ellipsized SSID> · <complete IPv4 address>
Data: adsb.fi
```

The IPv4 address is never truncated. If the combined row does not fit, the SSID and IP use separate rows. The web interface also remains reachable through `http://airtrack.local` when mDNS is supported by the LAN.

## 8. ADS-B request and parsing pipeline

Request template:

```text
GET https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{radius_nm}
```

Rules:

- Default poll interval: 5 seconds.
- Accepted configuration range: 2-300 seconds; never issue parallel requests.
- Enforce a monotonic start-to-start floor of 1,100 ms across normal polls, retries, tests, and configuration changes.
- Radius: integer 1-250 NM. All UI units convert to NM before request construction.
- Format coordinates with six decimal places. Send `Accept: application/json`, `Accept-Encoding: identity`, and a versioned AirTrack `User-Agent`.
- Verify the server with the ESP certificate bundle; never set skip-CN or insecure flags.
- Follow at most two same-host HTTPS redirects.
- Reuse the HTTP client where safe, but close and recreate it after transport/protocol failures.
- Network/DNS/TLS/timeouts, malformed responses, and 5xx use exponential backoff capped at 300 seconds with +/-20% jitter.
- A 429 honors `Retry-After`, otherwise waits at least 60 seconds and increases to a 15-minute cap. Repeated 400/401/403/404 responses latch an API/config error and retry no more than every 15 minutes until configuration or firmware changes.
- A successful manual settings test uses the same global rate limiter as normal polling.

### Bounded parser

Read response chunks directly from `esp_http_client` through a 2 KiB receive buffer. A lexical stream state machine locates the top-level `ac` array, respects quoted strings/escapes, and isolates one aircraft object at a time into a 4,096-byte buffer. Parse that single object with pinned cJSON, update a fixed top-five candidate set, free it, then reuse the buffer.

Structural limits are 8 MiB of streamed body bytes, 8,192 immediate aircraft objects, 4,096 bytes per object, and nesting depth 16. An oversized object/body, duplicate consumed key, second top-level `ac`, malformed/truncated JSON, premature close, or structural-limit violation invalidates the complete poll. Do not publish a candidate set that may have skipped the true nearest aircraft. A new snapshot is published only after the top-level object closes cleanly and the HTTP body completes successfully.

Retain only:

- `hex`, `flight`, `r`, `t`, and optionally a bounded `desc`.
- `lat`, `lon`, `dst`, `dir`, `seen_pos`, and `seen`.
- `alt_baro`, `alt_geom`, `baro_rate`, `gs`, `track`.
- `type`, `category`, `squawk`, and `emergency` where present.

All JSON fields are optional. Strings are bounded and sanitized, callsigns are trimmed, non-finite numbers are rejected, and `alt_baro` explicitly supports either a number or the string `"ground"`.

### Candidate selection

1. Require a valid identity and position.
2. Reject non-finite/negative `seen_pos` or positions older than configured `max_position_age_s` (default 15 seconds). When API time is plausible, add response transit time to `seen_pos`; displayed age then advances from monotonic time.
3. Exclude an explicit `alt_baro: "ground"` by default. Unknown ground state remains eligible.
4. Use nonnegative `dst` and normalized `dir` when valid; otherwise calculate Haversine distance and initial true bearing from the configured location. Reject candidates beyond configured radius + 0.1 NM.
5. Rank by distance, then fresher effective position age within a 0.001 NM tie, then ICAO hex for deterministic order.
6. Keep the nearest five for the web UI and logging.
7. Prevent flicker with a two-poll switch confirmation unless the challenger is at least 10% closer or the current target is stale/gone.

Callsign display fallback is trimmed `flight`, then registration `r`, then uppercase `hex`.

`NO RECENT REPORTS` is only shown after a valid HTTP 200 response and successful parse with no accepted candidates. Transport or parse errors show a distinct stale/offline state and retain the last good target with its age.

Every request carries the configuration revision used to build it. If that revision changes before completion, validate and drain the response but discard its result, then schedule a new rate-limited request.

## 9. Data contracts

### Configuration V1

The persisted configuration is a bounded UTF-8 JSON payload inside an explicitly serialized little-endian envelope; never `memcpy` a compiler struct to flash. The envelope contains `ATRK` magic, envelope format, payload schema, 64-bit generation, payload length (maximum 2,048 bytes), and CRC32 over the header-with-zeroed-CRC plus payload.

Store alternating NVS blobs `cfg_a` and `cfg_b` and load the highest valid generation. On update, write the older/invalid slot, commit, reread, and verify it before publishing the new in-RAM configuration. Interrupted updates therefore yield exactly the old or new record. Migrations form a one-way version chain and never overwrite an unknown future schema.

| Field | Type/default | Validation |
|---|---|---|
| Wi-Fi SSID/password | bounded strings | SSID 1-32 bytes; password per Wi-Fi security mode |
| Hostname | `airtrack` | DNS-safe, 1-24 characters |
| Latitude/longitude | unset signed E7 integers | -900000000..900000000 / -1800000000..1800000000 |
| Radius | 25 NM | 1..250 NM |
| Poll interval | 5 s | 2..300 s |
| Max position age | 15 s | 5..120 s |
| Include ground | false | boolean |
| Distance units | NM | NM, km, or statute miles |
| Brightness | 45% | 0..50%; never exceed hardware limit |
| SD logging | off | off, target changes, or interval |
| Log heartbeat | 60 s | 30..3600 s |
| Retention | 30 days / 64 MiB | bounded positive values |

The provisioning/admin secret is stored separately. `GET` APIs return `wifi_configured` and SSID but never the Wi-Fi or provisioning password. Configuration exports are redacted. No password may appear in status JSON, logs, serial output, coredump annotations, or generated error text.

### Aircraft snapshot

Use fixed arrays and explicit validity bits instead of heap-allocated strings or sentinel values. A snapshot contains:

- Monotonic receive time and server `now`, when valid.
- Query location/radius generation.
- HTTP status, API/parse health, total reported, accepted, and skipped counts.
- Up to five normalized aircraft.
- Last-success age and current backoff.

This single contract feeds the display, web API, and SD logger.

## 10. Local HTTP interface

All endpoints are versioned under `/api/v1`.

The connected-mode implementation serves a self-contained dashboard at
`GET /` (HTML generated on the device, `/app.css` and `/app.js` embedded from
`components/web/assets/`), plus bounded JSON status/config/aircraft snapshots.
It starts only after the station obtains an IPv4 lease, stops immediately if
that lease is lost, and is stopped before the captive setup AP starts. Its
data contract cannot receive or expose Wi-Fi/setup passwords. Tracker and
display settings (location, radius, ground filter, poll interval, units,
brightness, SD logging) are editable from the dashboard at any time via
`POST /api/v1/config`, guarded by a per-start CSRF token, the canonical numeric
`Host`, and a form content-type check, and are applied live without a restart.
`POST /api/v1/reboot` restarts the device with the same guards. Holding BOOT
for five seconds opens the isolated WPA2 setup portal for Wi-Fi changes.

| Method/path | Purpose |
|---|---|
| `GET /api/v1/status` | Firmware, uptime, mode, SSID/IP/RSSI, API, SD, heap, reset cause |
| `GET /api/v1/config` | Redacted effective configuration with generation ETag |
| `POST /api/v1/config` | Save and live-apply tracker/display settings (CSRF + Host) |
| `POST /api/v1/reboot` | Controlled restart (CSRF + Host) |
| `GET /api/v1/aircraft` | Current nearest/top-five snapshot |
| `GET /api/v1/wifi/scan` | Return bounded cached scan results and age |
| `POST /api/v1/wifi/scan` | Queue a scan job and return `202` |
| `POST /api/v1/wifi/connect` | Stage/test credentials in AP+STA mode; return a job ID |
| `GET /api/v1/jobs/{id}` | Report pending/testing/stable/committed/failed state |
| `POST /api/v1/provision` | Test and commit Wi-Fi plus initial tracker settings |
| `GET /api/v1/logs` | List log files (name, bytes) and total usage |
| `GET /api/v1/logs/{name}` | Tail (`?tail=bytes`, default 48 KiB) or download (`?download=1`) a validated log filename |
| `POST /api/v1/factory-reset` | Deferred: authenticated confirmed settings erase |
| `POST /api/v1/ota` | Deferred: signed firmware upload with rollback |

Static assets are dependency-free, built ahead of time, gzip-compressed, and embedded in flash. No CDN or Internet-hosted asset is required to configure the device. The dashboard polls bounded status/job endpoints every 2-5 seconds; v1 does not keep WebSocket or SSE connections open.

Limit request bodies to 4 KiB, cap simultaneous HTTP clients at three, enable least-recently-used socket eviction, bound JSON nesting/tokens, and return `Cache-Control: no-store` for API responses.

Read-only status may be available to the LAN. Mutating endpoints require a per-device authenticated session, an origin/CSRF check, bounded bodies, and content-type validation. The AP's WPA2 password is not treated as the sole authorization boundary once the device joins a LAN.

New Wi-Fi credentials are staged while the setup AP remains available. Commit only after the station interface has continuously held an IPv4 address for 30 seconds; keep the AP alive for another 10 seconds so the browser can receive the success result. A failed test leaves the previous credentials untouched.

## 11. Display views

1. Boot: firmware version and staged hardware/network checks.
2. Setup: `SCAN TO CONNECT`, real Wi-Fi QR, SSID, password, and `192.168.4.1`.
3. Connecting/time sync: target SSID, retry count/backoff, and setup hint.
4. Live target: identity, distance, north-relative bearing, altitude/rate, speed, freshness.
5. No reports: `NO RECENT REPORTS within <radius>` after a healthy empty response.
6. Stale/API offline: last target dimmed with explicit age and error category.
7. System: IP, RSSI, heap, uptime, firmware, and SD health.

Every station-connected view reserves the bottom network/attribution footer. Redraw only values that changed and update freshness at 1 Hz; the only continuous animation is the slow radar sweep on the no-reports screen. `tools/host_ui_render/render.sh` renders every screen on the host for review.

Backlight behavior:

- Keep it off until the first valid frame has been transferred.
- Use LEDC PWM on GPIO22 at approximately 5 kHz.
- Clamp all paths, including defaults and API input, to 50%.
- Optionally blank after a configured idle schedule, but do not use light sleep in the MVP.

## 12. SD logging

- Mount `/sd` as FAT32 with `format_if_mount_failed = false`.
- Absence, corruption, unsupported exFAT, or a full card is non-fatal.
- V1 detects I/O failure/removal, closes logging, and marks SD degraded; arbitrary hot reinsertion is not supported until reboot because the card must be initialized before LCD traffic.
- Log normalized sightings as append-only UTF-8 NDJSON under `/sd/airtrack/logs/`; never archive raw API payloads.
- Default logging is off. When on, every distinct aircraft entering the tracked set is written once per 30-minute window ("sighting"); the periodic mode also writes a "heartbeat" for the nearest aircraft every `log_heartbeat_s`.
- Records carry hex, callsign, registration, type, route, distance/bearing, altitude, speed, track, vertical rate, squawk, and emergency.
- Before SNTP, write `unsynced-<boot-count>.ndjson` with `ts:null` and monotonic time. Once synchronized, use `YYYY-MM-DD-00.ndjson`.
- Flush state transitions immediately; flush ordinary heartbeats within 30 seconds and `fsync` at least every 60 seconds.
- Rotate at UTC midnight or 8 MiB, incrementing the numeric suffix.
- Prune only strictly matching files inside `/sd/airtrack/logs`, oldest first, whenever the size cap (`retention_mib`, editable on the dashboard) or `retention_days` is exceeded; the check runs after roughly every 256 KiB written. Unrelated card contents are never touched.
- Validate all download filenames and never expose arbitrary filesystem paths.

Example record:

```json
{"v":1,"ts":"2026-08-16T21:31:04.218Z","mono_ms":382101,"event":"target","reason":"changed","hex":"461f39","flight":"FIN1855","reg":"OH-LVL","type":"A319","dst_nm":5.265,"dir_deg":241.5,"alt_ft":3725,"ground":false,"gs_kt":229.0,"track_deg":238.39,"vr_fpm":2048,"seen_pos_s":0.48}
```

One storage task owns all FATFS calls. Producers use a bounded queue, reserve capacity for state transitions, drop heartbeats first on overflow, and increment a diagnostic counter. Readers tolerate one partial final line after abrupt power loss.

## 13. Flash and RAM budgets

Connected-device 8 MB partition table:

```text
nvs       data nvs      0x009000 0x006000
otadata   data ota      0x00F000 0x002000
phy_init  data phy      0x011000 0x001000
ota_0     app  ota_0    0x020000 0x3D0000
ota_1     app  ota_1    0x3F0000 0x3D0000
coredump  data coredump 0x7C0000 0x040000
```

Each app slot is 3,904 KiB. The initial release target is no more than 3.25 MiB, and CI fails above 3.5 MiB, preserving at least 320 KiB for IDF, certificate, and feature growth. Web files, fonts, icons, and certificate choices are part of this budget. The public Waveshare page may still describe a C6FH4/4 MB variant, so release images are explicitly hardware-specific and the flash ID is verified before first installation.

OTA later streams a size-bounded, signed image directly into the inactive slot and enables bootloader rollback. Mark a candidate image valid only after local NVS, display-driver, Wi-Fi-driver, web-server, and heap self-tests pass; do not require adsb.fi or SD availability to accept otherwise healthy firmware. If both slots fail, native USB BOOT+RESET remains the recovery path.

RAM rules:

- Two 172 x 20 x 16-bit display buffers consume 13,760 bytes total.
- Do not allocate a 110,080-byte full framebuffer or a whole JSON document.
- Disable unused BLE and IEEE 802.15.4 stacks.
- Keep web assets in flash and serve them in bounded chunks.
- Enable mbedTLS dynamic buffers while retaining a safe 16 KiB inbound TLS record capacity until real adsb.fi testing proves a smaller value is interoperable.
- Measure task stack high-water marks and TLS peak heap on hardware.
- Acceptance floors: at least 80 KiB internal 8-bit heap and a 45 KiB largest block at normal idle; at least 40 KiB minimum-free and a 24 KiB largest block during TLS plus a dashboard request; at least 1 KiB and approximately 25% stack margin per task.

## 14. Diagnostics and recovery

Track and expose:

- Reset reason, firmware/IDF version, uptime, free/minimum heap, task stack watermarks.
- Wi-Fi reconnects, RSSI, current SSID/IP, DHCP changes.
- Last DNS/TLS/HTTP result, response bytes, parse skips, accepted aircraft, latency, backoff.
- SD mount/write errors and last successful flush.
- Maximum display flush latency and queue overruns.

Register the supervisor, ADS-B worker, LVGL task, and storage task with the task watchdog where appropriate. Feed the watchdog only after useful progress, with HTTP timeouts bounded below the measured watchdog interval. A recoverable component failure changes state and retries; it does not reboot the device. Display failure must not prevent AP/STA/web recovery, and SD failure must not prevent tracking.

Keep a reset-loop counter in RTC-retained memory. Three watchdog/panic resets before five healthy minutes enter safe mode: skip SD and ADS-B, start the QR recovery AP, keep a minimal display if available, and expose diagnostics/OTA. A healthy five-minute run clears the counter.

## 15. Verification plan

### Host tests

- Parser: empty response, one aircraft, dense response, every possible input chunk boundary, escaped quotes/braces, missing/null fields, `alt_baro: "ground"`, oversized object, malformed JSON, and truncated transfer.
- Tracker: distance/bearing fallback, units, ground/freshness filters, deterministic ties, hysteresis, target disappearance.
- Config: validation boundaries, CRC rejection, A/B generation selection, power-loss simulation, V1 migration, secret redaction.
- QR: escaping of reserved characters and maximum payload/module-size checks.
- HTTP: schema, body limits, authentication, path traversal rejection, and rate limiter.

### Hardware tests

- Color bars, portrait orientation, 34-pixel offset, BGR correctness, backlight 0/25/50%.
- Concurrent LVGL updates and repeated SD reads/writes on the shared bus.
- Boot with no SD, FAT32 SD, unsupported filesystem, corrupt card, full card, and card removal.
- QR connection and captive portal using multiple Android/iOS devices at realistic distances.
- No credentials, wrong password, absent AP, router restart, DHCP lease/IP change, long SSID, and manual BOOT recovery.
- SNTP unavailable, DNS failure, bad TLS time, 429, 5xx, chunked responses, and response interruption.
- Dense 250-NM response without heap growth proportional to aircraft count.
- Repeated abrupt power loss while saving config and flushing logs.
- 72-hour soak with heap/stack telemetry and no upward error/reconnect trend.

### MVP acceptance gates

- Setup QR appears within 5 seconds when no credentials are configured and within 65 seconds when a saved network remains unavailable.
- QR connects a phone to the correct WPA2 hotspot; manual credentials also work.
- Captive setup remains usable throughout a failed station connection attempt.
- SSID and complete IPv4 address remain visible at the bottom of every connected screen.
- A normal configured boot reaches tracking without user interaction.
- No adsb.fi request is made faster than the configured/global rate limit.
- Empty healthy data, stale data, Wi-Fi failure, and API failure are visually distinct.
- The device tracks without an SD card and never formats one automatically.
- Configuration survives power cycling and rejects a corrupt newest record.
- Firmware remains below the size gate and passes the 72-hour soak.

## 16. Delivery sequence

### Phase 0: reproducible skeleton — complete

- Initialize Git, ESP-IDF 5.5.5 project, component dependencies, CI build, formatting, host-test runner, partition table, and size gate.
- Exit gate: clean clone builds a flashable empty application and reports size.

### Phase 1: board support package — complete

- Implement shared SPI2, optional SD-first initialization, ST7789 panel, LVGL strips, backlight, RGB, BOOT input, and USB console.
- Exit gate: hardware diagnostic screen, correct geometry/colors, simultaneous SD/display stress.

### Phase 2: configuration and connectivity — complete

- Implement NVS A/B config, STA manager, AP+STA fallback, runtime QR, captive DNS/HTTP, SNTP, mDNS, BOOT long press, and persistent SSID/IP footer.
- Exit gate: a blank device can be provisioned entirely from a phone and recovers when its router disappears.

### Phase 3: ADS-B vertical slice — complete

- Implement verified HTTPS, global rate limiter, bounded parser, tracker, snapshots, backoff, and fixtures.
- Exit gate: live nearest-aircraft data reaches a serial diagnostic and survives dense responses/failures.

### Phase 4: production display states — complete

- Implement live/no-reports/stale/system views, freshness, attribution, target switching, and conservative redraws.
- Exit gate: every app/network/API state maps to an unambiguous screen.

### Phase 5: web dashboard and settings — core complete

- Implemented: dependency-free dashboard, status/config/aircraft APIs, one-time
  initial LAN location save, and isolated setup-portal Wi-Fi/location/radius
  mutation.
- Deferred: authenticated general LAN settings mutation and candidate test jobs.

### Phase 6: storage and resilience — core complete

- Implemented: optional bounded log queue, target/interval NDJSON records,
  8 MiB rotation, shared-SPI exclusion, diagnostics, coredump partition, and
  network/setup recovery.
- Deferred: retention pruning, log downloads, full fault-injection matrix, and
  long-duration soak sign-off.

### Phase 7: update and release hardening — release gate complete; OTA deferred

- Implemented: release artifact build/hashes, size gate, host tests, transport
  and filesystem policy checks, versioned settings migration, security review,
  and initial target heap/stack telemetry.
- Deferred: signed OTA upload/download, boot rollback, and 72-hour soak.

## 17. Deferred features

- Onboard GPS for moving operation.
- Aircraft photographs, maps, and scheduled departure/arrival times (route enrichment via adsbdb.com is implemented).
- Historical charts beyond downloadable SD logs.
- BLE provisioning, Thread/Zigbee, and persistent AP mode.
- Safety-critical alerts or collision prediction.

## 18. Primary references

- Waveshare board documentation: https://docs.waveshare.com/ESP32-C6-LCD-1.47?variant=ESP32-C6-LCD-1.47
- Waveshare ESP-IDF instructions: https://docs.waveshare.com/ESP32-C6-LCD-1.47/Development-Environment-Setup-ESP-IDF
- Waveshare schematic: https://files.waveshare.com/wiki/ESP32-C6-LCD-1.47/ESP32-C6-LCD-1.47_schemetics.pdf
- Waveshare example package: https://files.waveshare.com/wiki/ESP32-C6-LCD-1.47/ESP32-C6-LCD-1.47-Demo.zip
- adsb.fi open-data API: https://github.com/adsbfi/opendata/blob/main/README.md
- readsb JSON field reference: https://github.com/wiedehopf/readsb/blob/dev/README-json.md
- ESP32-C6 SD-over-SPI sharing: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/peripherals/sdspi_share.html
- ESP-IDF v5.5.5: https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c6/

The official demo archive inspected for this plan had SHA-256 `4b960e8ccd58e54e60494a2a6da62531695c9152c9950153cad34acc496c4ee4`; re-audit vendor init code if that download changes.
