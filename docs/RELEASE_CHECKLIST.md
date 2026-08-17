# AirTrack 1.0.0 release checklist

This checklist distinguishes reproducible release gates from tests that need
the physical device, its real fixed location, or elapsed soak time.

## Passed for the current artifact

- [x] ESP-IDF 5.5.5 production build succeeds.
- [x] Host tracker/parser tests pass, including malformed and dense responses.
- [x] adsb.fi transport is HTTPS with certificate verification; redirects are
  disabled and polling cannot exceed the public one-request-per-second limit.
- [x] SD mount never auto-formats and absence/mount failure is non-fatal.
- [x] Image is below the 3.5 MiB gate: 1,529,536 bytes.
- [x] Both 3,904 KiB OTA app partitions retain 62 percent free.
- [x] Artifact SHA-256:
  `9bb12521776ccb51cc0012a53f41f55de0c8ee1791456b499e0ec7e4e17ab71c`.
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
- [ ] Run a 24-hour burn-in followed by the planned 72-hour soak with stable
  heap, adequate task stack watermarks, no watchdogs, and no reconnect trend.

## Deliberately deferred from 1.0.0

- Signed browser/remote OTA and automatic rollback. Dual slots are reserved,
  but 1.0.0 updates use native USB.
- Authenticated general-purpose settings mutation on the normal LAN. The LAN
  dashboard is read-only after the one-time initial location save; later
  changes use the physically requested isolated setup portal.
- Log listing/download, retention pruning, and arbitrary hot reinsertion of an
  SD card after boot.
- Flash/NVS encryption. Wi-Fi credentials are never served or logged, but are
  stored unencrypted at rest on this prototype hardware.

## Reproduce the artifact gate

```sh
source /home/skitty/esp/esp-idf-v5.5.5/export.sh
./tools/check_release.sh
```

The printed application hash must match the hash above for this exact
artifact. Any source or configuration change requires a new gate run, hash,
flash, and target smoke test.
