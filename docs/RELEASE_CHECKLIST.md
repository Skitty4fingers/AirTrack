# AirTrack 1.5.0 release checklist

This checklist distinguishes reproducible release gates from tests that need
the physical device, its real fixed location, or elapsed soak time.

## Passed for the current artifact

- [x] ESP-IDF 5.5.5 production build succeeds.
- [x] Host tracker/parser tests pass, including malformed and dense responses.
- [x] adsb.fi transport is HTTPS with certificate verification; redirects are
  disabled and polling cannot exceed the public one-request-per-second limit.
- [x] SD mount never auto-formats and absence/mount failure is non-fatal.
- [x] Image is below the 3.5 MiB gate: 1,770,640 bytes (both 3,904 KiB OTA
  slots retain 56 percent free).
- [x] Artifact SHA-256:
  `88c220a7ef79462f259265d15c097799ebb12d9d80cbbefb44ff51b1996fad75`.
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
  Dual slots are used by the dashboard updater since 1.6.0; 1.5.0 updates use native USB.
- Authenticated general-purpose settings mutation on the normal LAN. The LAN
  dashboard is read-only after the one-time initial location save; later
  changes use the physically requested isolated setup portal.
- Arbitrary hot reinsertion of an SD card after boot.
- Flash/NVS encryption. Wi-Fi credentials are never served or logged, but are
  stored unencrypted at rest on this prototype hardware.

## Publish an over-the-air release

```sh
GITHUB_TOKEN=... tools/publish_release.sh <version> "release notes"
```

The script refuses a dirty tree or a version that does not match
`CMakeLists.txt`, runs the gate below, uploads the artifact to the GitHub
Release, and pushes `docs/firmware/manifest.json` (that push is what makes
devices see the update).

## Reproduce the artifact gate

```sh
source /home/skitty/esp/esp-idf-v5.5.5/export.sh
./tools/check_release.sh
```

The printed application hash must match the hash above for this exact
artifact. Any source or configuration change requires a new gate run, hash,
flash, and target smoke test.
