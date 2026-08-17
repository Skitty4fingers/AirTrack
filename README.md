# AirTrack

AirTrack is self-contained firmware for the Waveshare ESP32-C6-LCD-1.47. It
connects to Wi-Fi, requests nearby traffic from adsb.fi, and shows the closest
fresh aircraft to a configured fixed location on the 172 x 320 display and a
local web dashboard.

This tree targets the connected ESP32-C6FH8 revision with 8 MB embedded flash
and no PSRAM. Verify the flash capacity before installing it on another board
revision. AirTrack is an enthusiast display, not a receiver, navigation aid,
or collision-warning device.

## Firmware status

The connected unit is running AirTrack 1.1.0. Changes since 1.0.0:

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
  `http://airtrack.local`, when mDNS is supported by the client;
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
