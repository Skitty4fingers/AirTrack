# AirTrack 1.8.0 release checklist

This checklist distinguishes reproducible release gates from tests that need
the physical device, its real fixed location, or elapsed soak time.

Everything recorded as passed below was measured on the **ESP32-C6-LCD-1.47**.
The ESP32-C6-Touch-LCD-2.8 port has not yet been run on hardware; its
acceptance is the separate section near the end, and no 2.8 image should be
published until that section passes.

## Passed for the current artifact

- [x] ESP-IDF 5.5.5 production build succeeds.
- [x] Host tracker/parser tests pass, including malformed and dense responses.
- [x] adsb.fi transport is HTTPS with certificate verification; redirects are
  disabled and polling cannot exceed the public one-request-per-second limit.
- [x] SD mount never auto-formats and absence/mount failure is non-fatal.
- [x] Both images are below the 3.5 MiB gate:
  - ESP32-C6-LCD-1.47: 1,806,800 bytes (55 percent of each 3,904 KiB slot free)
  - ESP32-C6-Touch-LCD-2.8: 1,805,600 bytes (71 percent of each 6 MiB slot free)
- [x] Artifact SHA-256:
  - `eb2c944f65babdd86cb5d386ce04fca158dc027006d2f2891812708e9ce42886`
    (`airtrack-1.8.0.bin`)
  - `c6965989456b9337d596b77a9a03f7a262ce63ff93865c00e737ffbaaa4d709a`
    (`airtrack-esp32c6-touch-lcd-2.8-1.8.0.bin`)
- [x] Browser-install factory image SHA-256 (bootloader + partition table +
  otadata + app merged at offset 0, byte-identical to the four release
  binaries):
  - `edcb791b88c3144790ffa5ae83b90f3931d82697eab71f5f290d2befaabcf06e`
    (`airtrack-1.8.0-factory.bin`)
  - `59f9b4580d35f3e9a64522cd75063d377885e7de164654b07ff27661faf661bf`
    (`airtrack-esp32c6-touch-lcd-2.8-1.8.0-factory.bin`)
- [x] Published release assets verified against their manifests after upload:
  size and SHA-256 of both boards re-downloaded from the GitHub Release and
  compared to the committed manifest values.
- [x] 1.7.0 on-target (2.8): I/O expander answered at 0x24, panel came up at
  240x320 and 40 MHz, PCF85063A seeded the clock before Wi-Fi, SHTC3 read
  through to the dashboard, station joined, mDNS answered, and the feed
  reached `live`. Host tracker tests pass with `-Wall -Wextra -Werror`.
- [x] 1.6.3 on-target: a dashboard update check fetched the manifest over
  HTTPS and reported up to date with the release date, check age, and
  multi-line notes rendered in the Updates card.
- [x] 1.6.x on-target: a 1.6.0 unit updated itself to 1.6.1 from the dashboard
  (progress to 100 percent, restart into the other slot, running image marked
  valid), and a second unit was installed from the merged factory image.
- [x] 1.5.0 on-target: sighting-window select saved (30 -> 1440 -> 30),
  `POST /api/v1/logs/clear` removed the day file (bad token refused), and
  `POST /api/v1/factory-reset` without the typed word was refused with the
  device staying up.
- [x] 1.4.0 on-target: night mode reported active inside a test window and
  inactive after restoring 23:00-07:00 America/Los_Angeles; a sighting for
  UAL2058 was held and logged with `route:"IAD-IAH"`.
- [x] 1.3.0 on-target: adsbdb route lookups resolve airline callsigns (e.g.
  SKW3363 -> LAX) and cache unknown GA registrations; focus mode set/cleared
  from the API narrows the tracked set to one aircraft; the sighting log
  writes `YYYY-MM-DD.ndjson` records and `/api/v1/logs` lists/tails them
  (path traversal and NUL names rejected with 404).
- [x] Host LCD render (`tools/host_ui_render/render.sh`) reviewed for the
  live, stale, emergency, empty, no-Wi-Fi, and both setup screens against
  `docs/ui/device-states-concept-v2.png`.
- [x] Dashboard reviewed in headless Chromium at 1400 px and 420 px widths
  against `docs/ui/web-configurator-concept.png`; settings save round trip
  (CSRF rejection, live apply of radius/units/brightness, revert, unknown-field
  rejection, no-JS redirect) verified with curl on the connected unit.
- [x] Connected hardware identified as ESP32-C6FH8 revision 0.2 with 8 MB
  flash before installation.
- [x] Existing NVS was backed up before the first 1.0.0 installation; the
  corrected flash did not erase NVS.
- [x] Hardware boot passed: LCD, 28.9 GiB FAT32 SD, saved Wi-Fi, DHCP, SNTP,
  mDNS, and LAN dashboard all initialized.
- [x] The dashboard root and `/api/v1/status`, `/api/v1/config`, and
  `/api/v1/aircraft` returned valid bounded responses.
- [x] Supervisor stack retained 4,516 bytes after dashboard startup.
- [x] Repeated LAN requests held free heap near 133.8 KiB with a 125.6 KiB
  recorded minimum during the unconfigured-location smoke test.
- [x] 1.1.0 on-target: configured location polls adsb.fi over one reused TLS
  session (`tls_connections` stays at 1 across hundreds of polls), the
  published feed state no longer flickers through STALE between polls, idle
  free heap ~118 KiB with a ~94 KiB minimum, and the forced recovery hook
  (`AIRTRACK_TEST_FORCE_RECOVERY_MS`) completed a full tracking -> AP+STA
  recovery -> tracking round trip in about 14 seconds.

## Required before the first ESP32-C6-Touch-LCD-2.8 release

The port is written against Waveshare's published pin map and its Arduino
board-support sources. None of it has been executed on the board, so treat
every item here as unverified rather than expected-to-pass. Work down the list
in order: each step depends on the one above it.

Bring-up:

- [ ] `tools/check_release.sh --board esp32c6-touch-lcd-2.8` completes, and the
  image fits the gate. Record the image size and SHA-256.
- [ ] Serial log shows `board_exio: I/O expander ready at 0x24`. If it does
  not, nothing else on this list can pass: the panel reset and backlight both
  depend on that device answering.
- [ ] Serial log shows `board_lcd: ST7789 240x320 ready at 40 MHz`, and the
  panel actually lights.
- [ ] Backlight tracks the dashboard slider and the night schedule. The duty
  register is 0-255 and AirTrack clamps to 50 percent, so full brightness
  should write 128.
- [ ] Colours are correct, not inverted or byte-swapped, and the image is not
  mirrored or offset. `BOARD_LCD_MIRROR_X`, `BOARD_LCD_MIRROR_Y`, and the
  `0x21` (INVON) entry in the init table are the knobs if it is wrong.
- [ ] Try `st7789_d0_param_count` at both 1 and 2. The 2.8 defaults to the
  Arduino behaviour (both bytes) because that is what its only vendor demo
  does; confirm that is right on real hardware.
- [ ] The panel clock is set to 40 MHz against the vendor demo's 80 MHz,
  because SPI2 is shared with the SD card. Confirm the display is clean at 40,
  then decide whether to raise it after a combined LCD+SD soak.

Shared SPI bus:

- [ ] A FAT32 card mounts (`board_sd`, CS GPIO23, MISO GPIO8) and is not
  formatted.
- [ ] Sustained sighting logging while the display refreshes shows no
  corruption on either device, which is what the SPI gate exists to prevent.
- [ ] Removing the card leaves tracking running.

Sensors:

- [ ] Serial log shows `sensors: SHTC3 present`, and the dashboard System card
  shows a plausible `On-board climate` row. `/api/v1/status` carries
  `temperature_c` and `humidity_percent`.
- [ ] Serial log shows the PCF85063A. After SNTP syncs, power-cycle with the
  router unreachable and confirm `Clock seeded from the on-board RTC` and that
  the device does not sit in TIME_SYNC.
- [ ] A cold board with no held time logs the stopped-oscillator message and
  falls back to SNTP without erroring.

Updates, which is where a mistake is most expensive:

- [ ] `/api/v1/status` reports `"board":"esp32c6-touch-lcd-2.8"`.
- [ ] Point the device at the 1.47 manifest and confirm the check **fails**
  with `manifest is for esp32c6-lcd-1.47` and does not offer the version. This
  is the check that stops a cross-board install; verify it before publishing
  anything.
- [ ] A 1.47 unit still updates from its own untagged and board-tagged
  manifests, unchanged.
- [x] A full 2.8 update round trip: download, verify, restart into the other
  slot, self-test passes, image marked valid. Done on 1.6.3 -> 1.7.0: the
  device moved from `ota_0` to `ota_1`, reported `pending_verify` false, and
  came back up tracking.
- [ ] The browser installer's board selector installs a blank 2.8 and the
  device comes up in setup mode.

UI at 240 pixels:

- [ ] `AIRTRACK_BUILD_DIR=build-production-touch28
  tools/host_ui_render/render.sh build-host-ui-touch28` renders every screen,
  and the compass, distance, radar, and route columns are centred with nothing
  clipped or overlapping.
- [ ] Same check on the real panel for the live, empty, stale, offline,
  updating, and both setup screens.
- [ ] The setup QR scans from a phone at the larger size.

Battery:

- [x] With USB attached the sense rail reads ~4.14 V and `usb_present` is
  true; the level is withheld in favour of "USB power", because the charger
  holds the rail near full whether or not a cell is fitted.
- [x] With USB removed the board stays up on the cell, `usb_present` goes
  false, and the reading settles to ~4.11 V.
- [ ] Confirm the scaling against a meter across the cell while on battery.
  The 10-bit / 3:1 figures are inferred, not measured; `battery_adc_counts`
  in `/api/v1/status` is exposed for exactly this.
- [ ] Run the cell down far enough to confirm the percentage tracks the
  discharge curve rather than only the top of it.
- [ ] Confirm behaviour on a charger that does not enumerate as a USB host:
  expected to read as battery power and quote a charger-held level.

Expected differences, to confirm rather than treat as faults:

- [ ] No status LED. `board_rgb_*` returns `ESP_ERR_NOT_SUPPORTED`, the System
  card shows no LED state, and the night-mode "LED off" option does nothing.
- [ ] The touch panel does nothing. AirTrack has no touch input.

## Required before final field sign-off

- [ ] Enter the device's actual fixed latitude, longitude, and radius on its
  LAN page. Do not use placeholder coordinates.
- [ ] Confirm an on-target TLS poll returns a healthy adsb.fi response and the
  nearest aircraft appears on both LCD and web dashboard.
- [ ] Confirm the SSID and complete IPv4 address remain visible in every live,
  empty, stale, and offline LCD state.
- [ ] Power-cycle once and verify Wi-Fi plus tracker settings survive.
- [ ] Temporarily remove or replace the SD card and confirm tracking continues
  without formatting or reboot loops.
- [ ] Test the five-second BOOT hold, QR join, captive-page popup, nearby-SSID
  selection, rejected password, and successful recovery from a phone.
- [ ] Disconnect Internet while retaining Wi-Fi and verify stale/offline state
  without exposing the setup portal on the station LAN.
- [ ] Run Factory reset from the dashboard on a bench unit and confirm it
  returns to setup mode with a new hotspot password and an empty SD log
  directory (not exercised on the connected unit to preserve its settings).
- [ ] With the night window set to include the current local time, confirm
  the panel dims and the LED goes dark; restore the real window afterwards.
- [ ] Confirm the accessory LED is blue while tracking and orange in setup or
  when the feed is stale/offline.
- [ ] Set *Track a single flight* to a nearby airliner callsign and confirm the
  LCD switches to `WAITING FOR` / the focused view with route and ETA.
- [ ] Power the router off for more than 60 seconds and confirm the recovery
  setup screen appears, then that tracking resumes by itself once the router
  is back (no power cycle).
- [ ] Run a 24-hour burn-in followed by the planned 72-hour soak with stable
  heap, adequate task stack watermarks, no watchdogs, and no reconnect trend.

## Deliberately deferred from 1.0.0

- Signed OTA images (deliberately left out; HTTPS + manifest SHA-256 instead).
  The dashboard updater has used both OTA slots since 1.6.0; a blank board is
  installed from `docs/flash.html` or over native USB.
- Authenticated general-purpose settings mutation on the normal LAN. The LAN
  dashboard is read-only after the one-time initial location save; later
  changes use the physically requested isolated setup portal.
- Arbitrary hot reinsertion of an SD card after boot.
- Flash/NVS encryption. Wi-Fi credentials are never served or logged, but are
  stored unencrypted at rest on this prototype hardware.

## Publish an over-the-air release

```sh
GITHUB_TOKEN=... tools/publish_release.sh <version> "release notes"
GITHUB_TOKEN=... tools/publish_release.sh --board esp32c6-touch-lcd-2.8 \
    <version> "release notes"
```

Each board is published separately and independently; publishing one does not
touch the other's manifest or factory image.

The script refuses a dirty tree or a version that does not match
`CMakeLists.txt`, runs the gate for that board, uploads the artifact to the
GitHub Release, and pushes the board's manifest (that push is what makes
devices see the update). It also merges the bootloader, partition table,
otadata, and app into the board's factory image, updates the matching
`web-flash` manifest, and drops that board's previous factory image, which is
what the browser installer (`docs/flash.html`) writes over USB.

| | 1.47 | Touch 2.8 |
|---|---|---|
| OTA manifest | `manifest.json` | `manifest-esp32c6-touch-lcd-2.8.json` |
| Installer manifest | `web-flash.json` | `web-flash-esp32c6-touch-lcd-2.8.json` |

Every published manifest carries a `board` field. Devices refuse one that
names different hardware, so a mis-pointed manifest is a failed check rather
than an install that leaves the panel dark.

After publishing, check the installer page:

- [ ] <https://skitty4fingers.github.io/AirTrack/flash.html> shows the new
  version and its release notes.
- [ ] In Chrome or Edge on a desktop, **Install** lists the device, erases,
  writes, and the unit reboots into the setup hotspot.

## Reproduce the artifact gate

```sh
source /home/skitty/esp/esp-idf-v5.5.5/export.sh
./tools/check_release.sh
```

The printed application hash must match the hash above for this exact
artifact. Any source or configuration change requires a new gate run, hash,
flash, and target smoke test.
