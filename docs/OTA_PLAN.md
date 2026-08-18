# AirTrack over-the-air updates

Status: implemented in 1.6.0 (`components/ota`, System-card UI,
`tools/publish_release.sh`). This page is the design reference.

Update a deployed AirTrack from its LAN dashboard, with firmware packages
kept on GitHub. No signing, no channels, no automation beyond one script.

## How it fits together

```
bench: tools/publish_release.sh 1.6.0
   ├─ builds via tools/check_release.sh (gate + hash)
   ├─ creates GitHub Release v1.6.0, uploads airtrack-1.6.0.bin
   └─ writes docs/firmware/manifest.json, commits, pushes  → Pages redeploys

device dashboard, System card
   "Check for updates" → GET https://skitty4fingers.github.io/AirTrack/firmware/manifest.json
   "Install 1.6.0"     → stream image into the inactive OTA slot, check size + SHA-256,
                         switch boot partition, restart
   new image boots, passes self-test, marks itself valid (else bootloader rolls back)
```

## Packages on GitHub

- **Image**: a GitHub Release per version (`v1.6.0`) with one asset,
  `airtrack-1.6.0.bin` — the exact `build-production/airtrack.bin` from the
  release gate. Binaries stay out of git history.
- **Manifest**: `docs/firmware/manifest.json`, served by GitHub Pages at
  `https://skitty4fingers.github.io/AirTrack/firmware/manifest.json`:

  ```json
  {"version":"1.6.0","size":1774240,"sha256":"e9817ec2…",
   "url":"https://github.com/Skitty4fingers/AirTrack/releases/download/v1.6.0/airtrack-1.6.0.bin",
   "notes":"Night schedule, route fixes."}
  ```

  Editing this file is the whole "publish" and "unpublish" mechanism.
- **Publishing**: `tools/publish_release.sh <version>` on the bench does the
  build, the release, the asset upload (GitHub REST API with your token from
  the environment), the manifest commit, and the push. Optional later: the
  same steps as a GitHub Actions workflow on tag push.

## Device

- New `components/ota`: fetch manifest (bounded, cJSON), compare with the
  running version, download with `esp_https_ota` into the inactive slot
  (streaming SHA-256 + size check against the manifest), set boot partition,
  restart. Any failure leaves the current image untouched and reports why.
- Supervisor: pause ADS-B polling and route lookups during the download (frees
  the TLS heap; `esp_https_ota` needs ~45 KiB peak, ~100 KiB is free idle);
  LCD shows `UPDATING 62%` then `RESTARTING`; LED orange.
- Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; the new image marks
  itself valid after NVS load + LCD + IPv4 lease + dashboard up + heap ≥ 60 KiB
  within 3 minutes, otherwise the bootloader boots the previous slot. This is
  a bootloader option, so each unit gets **one** more USB flash (bootloader +
  first OTA-capable firmware) — both are on the bench now.
- Dashboard **Updates** card (own card since 1.6.3): *Check for updates* →
  installed vs latest tiles (with the manifest's `released` date and how long
  ago it was checked), status line, release notes block, *Install x.y.z* →
  progress bar → reconnect and reload on the new version. Endpoints:
  `POST /api/v1/ota/check`, `GET /api/v1/ota/status`, `POST /api/v1/ota/start`
  (CSRF, canonical Host, as every other mutation). Manifest `notes` may
  contain newlines and up to 480 bytes.
- Only newer versions are offered; settings/NVS are never touched.

## Security in one paragraph

Manifest and image are fetched over HTTPS verified with the ESP certificate
bundle (GitHub / github.io / githubusercontent.com only; the one release
redirect is followed only to those hosts). The manifest's SHA-256 and size
must match the streamed image before the boot partition changes. Rollback
covers an image that installs but misbehaves. That is sufficient for a
hobby device on a home LAN; image signing was considered and left out on
purpose.

## Delivery

1. Firmware: `components/ota`, LCD/LED integration, System-card UI, sdkconfig
   (`BOOTLOADER_APP_ROLLBACK_ENABLE`, `OTA_ALLOW_HTTP` off), host tests for
   version compare and manifest parsing; USB-flash both units once.
2. Repo: `tools/publish_release.sh`, `docs/firmware/manifest.json`, checklist
   update; publish `v1.6.0` and install it on one unit from the dashboard as
   the acceptance test.
3. Later, if wanted: Actions workflow, install-from-file fallback, daily
   auto-check with a badge.
