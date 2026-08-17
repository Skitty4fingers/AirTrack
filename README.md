# AirTrack

AirTrack is self-contained firmware for the Waveshare ESP32-C6-LCD-1.47. It
connects to Wi-Fi, requests nearby traffic from adsb.fi, and shows the closest
fresh aircraft to a configured fixed location on the 172 x 320 display and a
local web dashboard.

This tree targets the connected ESP32-C6FH8 revision with 8 MB embedded flash
and no PSRAM. Verify the flash capacity before installing it on another board
revision. AirTrack is an enthusiast display, not a receiver, navigation aid,
or collision-warning device.

## Screens

Rendered from the current firmware: the LCD frames come from
`tools/host_ui_render/render.sh` (the real LVGL UI code drawn on the host at
2x), the dashboard captures are headless-Chromium screenshots of the live
device. Design references are in [`docs/ui/`](docs/ui/).

**Device** — single-flight focus with route and ETA, no recent reports, Wi-Fi
lost, and setup (network names, addresses, and coordinates in all images are
sample values):

![AirTrack LCD states](docs/ui/lcd-states.png)

**Dashboard** (`http://<device-ip>/` or `http://airtrack.local/`), desktop and
phone widths:

<p>
  <img src="docs/ui/dashboard.png" alt="AirTrack dashboard, desktop" width="66%">
  <img src="docs/ui/dashboard-mobile.png" alt="AirTrack dashboard, phone" width="20%" align="top">
</p>

**Setup portal** (captive page on the `AirTrack-xxxx` hotspot) and the
**HTTPS locate helper** (served from this repo's GitHub Pages) that hands your
browser's location back to the dashboard:

<p>
  <img src="docs/ui/setup-portal.png" alt="Setup portal" width="34%" align="top">
  <img src="docs/ui/locate-helper.png" alt="Locate helper" width="34%" align="top">
</p>

## Firmware status

The connected unit is running AirTrack 1.5.0.

Changes in 1.5.0:

- SD sighting log: choose how often a distinct aircraft may be logged again
  (30 minutes, hourly, every 6 hours, or daily); a **Clear log** button
  deletes every log file (`POST /api/v1/logs/clear`, CSRF-guarded).
- **Factory reset** next to Restart: erases the SD sighting log and every
  stored setting (Wi-Fi, location, options, and the setup-hotspot identity, so
  a new password is generated), then restarts into setup mode. Requires the
  CSRF token and typing `RESET` (`POST /api/v1/factory-reset`).
- Settings schema 4 (adds the sighting window); older records migrate.

Changes in 1.4.0:

- Night schedule (Display card): between two local times (default 23:00-07:00)
  the panel dims to a separate night brightness (default 5%) and, optionally,
  the status LED switches off. Requires a timezone: pick one from the preset
  list (the browser's zone is pre-selected when none is saved) or enter a
  POSIX TZ rule; the System card shows the device's local time and whether
  night mode is active. Settings schema 3; older records migrate.
- Log records and the top-five table now carry the route; a sighting is held
  up to 20 s so the lookup can land before the record is written.
- `/api/v1/config` returns the focus flight, retention, night schedule, and
  timezone; `/api/v1/status` adds `night` and `local_minutes`.

Changes in 1.3.0:

- Route enrichment: for each callsign in the tracked set the ADS-B worker asks
  `api.adsbdb.com` once (bounded 12-entry cache, one lookup per poll, unknown
  callsigns remembered for an hour) and shows origin/destination codes beside
  the compass on the LCD (`FROM` / `TO` columns) and in the dashboard.
- Track a single flight: a callsign, registration, or ICAO hex under
  *Location → Track a single flight* limits tracking to that aircraft; the LCD
  shows `WAITING FOR <flight>` until it reports in range, then its route,
  distance to go, and an ETA estimated from ground speed. Scheduled
  departure/arrival times are not available from any free source; wire a paid
  schedule API if you need on-time status.
- Location helper: *Use my location* (browser geolocation) and a paste box
  that accepts `lat, lon` or a maps link with `@lat,lon`. Because browsers only
  share location on HTTPS pages, the button hands off to
  [`docs/locate.html`](docs/locate.html), served over HTTPS from GitHub Pages at
  <https://skitty4fingers.github.io/AirTrack/locate.html>; that page reads the
  position once and navigates back to the dashboard with `?lat=&lon=` filled
  in (only home-network return addresses are accepted).
- Accessory LED mirrors the display: blue while the feed is healthy, orange
  when connecting, stale, offline, or in setup.
- SD sighting log: every distinct aircraft entering the tracked set is written
  once per 30 minutes (plus optional heartbeats); files are capped by the
  configurable size limit (MiB) and retention days, oldest files pruned first;
  the Storage card lists log files and shows the newest records inline with a
  download link (`GET /api/v1/logs`, `GET /api/v1/logs/<name>[?tail=N|download=1]`).
  Long FAT filenames are now enabled (they were required but off in 1.0.0).
- Settings schema 2 (adds the focus flight); schema 1 records migrate on load.

Changes in 1.2.0 (bringing the device and web UI in line with the concepts in
`docs/ui/`):

- LCD tracking screen redesigned after `device-states-concept-v2.png`: header
  with Wi-Fi glyph, feed dot, and last-update age; centred callsign with
  `TYPE • REG`; large `3.2 NM` distance; a compass gauge with N/E/S/W, ticks,
  a bearing arrow and highlight arc, and a plane silhouette rotated to the
  aircraft's track; icon rows for bearing, altitude, vertical rate, and speed;
  and a two-line footer. The no-reports state shows a slowly sweeping radar
  and `within 25 NM`; the setup screen shows `SCAN TO CONNECT`, a larger QR,
  and amber SSID / password / address.
- LAN dashboard rebuilt after `web-configurator-concept.png`: light theme,
  sidebar (Dashboard / Location / Display / Storage / System), status bar
  (ONLINE, Wi-Fi, API state, updated-ago), a nearest-aircraft card with a
  live SVG compass and the five nearest aircraft, and *editable* settings
  cards — search location, radius slider, airborne-only toggle, refresh
  interval, SD sighting log, brightness slider, units — with a Save button
  that applies changes immediately (no restart), plus a System card with
  diagnostics and a Restart button. CSS/JS are real files under
  `components/web/assets/` embedded at build time.
- Policy change: tracker/display settings can now be changed from the LAN
  dashboard at any time (CSRF token + canonical-Host + content-type checks),
  not only once while unconfigured. Wi-Fi credentials remain changeable only
  from the isolated setup hotspot.

Changes in 1.1.0:

- the ADS-B worker keeps one HTTPS connection alive between polls instead of
  performing a full TLS handshake every few seconds (one TLS session per
  network session; ~10 KiB more free heap, faster polls, less load on
  adsb.fi);
- the published feed state no longer flickers LIVE/STALE around every request,
  and position age is measured from the last successful poll;
- losing the saved Wi-Fi now opens a *recovery* setup hotspot while the station
  keeps retrying in the background (every 20 s), and tracking resumes
  automatically once the network is back; a BOOT hold in setup mode restarts
  the device;
- redesigned LCD: 28 px callsign, 40 px distance, north-up bearing arrow with
  cardinal, altitude / vertical speed / ground speed / squawk grid, emergency
  highlighting, freshness line, and a footer with Wi-Fi state, RSSI, and the
  full IPv4 address;
- the LAN dashboard refreshes itself every 2 s from the JSON API and lists the
  five nearest aircraft; `/api/v1/aircraft` now carries squawk, category,
  emergency, unit, radius, and per-aircraft age; `/api/v1/status` reports poll
  and TLS-connection counters and SD logging state;
- the setup portal pre-fills the saved SSID, location, and radius (never the
  password) and exposes units, brightness, poll interval, ground filter, and SD
  logging under "Display & feed options";
- station RSSI is refreshed every 5 s instead of only at DHCP time;
- `tools/host_ui_render/render.sh` renders every LCD screen on the host so the
  layout can be reviewed without the hardware.

The implemented core includes:

- one serialized SPI2 bus shared safely by the ST7789 LCD and optional SD card;
- a portrait LVGL interface using two bounded 20-line DMA buffers;
- a backlight hard-limited to 50 percent;
- persistent CRC-protected A/B tracker settings and separately stored Wi-Fi
  credentials;
- a per-device WPA2 setup hotspot, Wi-Fi QR, captive portal, and selectable
  scan of up to 16 nearby networks;
- fixed location and 1-250 NM radius setup from the local web interface;
- automatic recovery setup after 60 seconds without the saved Wi-Fi, plus a
  five-second BOOT-button shortcut;
- a verified-HTTPS adsb.fi client with a bounded streaming parser, top-five
  ranking, freshness/ground filters, target hysteresis, rate limiting, and
  retry backoff;
- live, empty, stale, offline, and configuration-required LCD states, with the
  complete SSID and IPv4 address retained in the footer;
- a station-LAN dashboard at the address shown on the LCD and at
  `http://airtrack.local`, when mDNS is supported by the client, with
  editable tracker/display settings and a restart button;
- bounded JSON at `/api/v1/status`, `/api/v1/config`, and
  `/api/v1/aircraft`;
- SNTP time synchronization and optional bounded NDJSON logging to FAT32 SD;
  and
- host parser/tracker tests plus a repeatable security, filesystem, build,
  image-size, and artifact-hash release gate.

On the connected unit the setup SSID is `AirTrack-8134`. Its eight-character
password is shown only on the LCD and encoded in the QR. It is not returned by
the LAN API or written to logs.

If Wi-Fi is already configured but the tracking location is not, open the
numeric address shown at the bottom of the LCD and enter the actual fixed
latitude, longitude, and radius once. Afterward, hold BOOT for five seconds to
open the isolated setup hotspot when Wi-Fi or location/radius needs changing.

The current image is about 1.72 MB and leaves 57 percent of each 3,904 KiB
OTA app slot free. The partition table reserves dual OTA slots, but signed web
OTA and rollback are intentionally deferred; update over native USB.

## Build, verify, and flash

Use ESP-IDF 5.5.5:

```sh
source /home/skitty/esp/esp-idf-v5.5.5/export.sh
./tools/check_release.sh
idf.py -B build-production -p /dev/ttyACM0 flash monitor
```

The release check runs host tests, verifies the secure transport and
non-formatting SD policies, builds the production image, enforces the image
budget, and prints SHA-256 hashes. Flashing does not erase the NVS partition;
do not run `erase-flash` when preserving configured Wi-Fi and settings.

To review the LCD screens without the board, run
`tools/host_ui_render/render.sh` (needs a host C compiler and the `build/`
sdkconfig); it writes PNGs of every screen to `build-host-ui/`. To exercise the
Wi-Fi recovery round trip on a bench with the router still present, build once
with `AIRTRACK_TEST_FORCE_RECOVERY_MS=25000 idf.py -B build-recovery-test build`
and watch the serial log; never ship that image.

See [the firmware plan](docs/FIRMWARE_PLAN.md) for the architecture and
[the release checklist](docs/RELEASE_CHECKLIST.md) for target acceptance and
the remaining production sign-off tests.
