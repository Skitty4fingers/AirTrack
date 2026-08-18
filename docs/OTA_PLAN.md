# AirTrack over-the-air update plan

Goal: update a deployed AirTrack from its LAN dashboard, with firmware
packages published on GitHub, without ever bricking a unit or needing the USB
cable again after the one bootloader upgrade described below.

## 1. Shape of the system

```
GitHub                                   AirTrack (LAN)
------                                   --------------
tag v1.6.0 ──► Actions: build+sign ──►   Release v1.6.0
                 airtrack-1.6.0.bin        (asset, signed image)
                 airtrack-1.6.0.sha256
               ► docs/firmware/manifest.json  (GitHub Pages, HTTPS)
                                          │
Dashboard ─ "Check for updates" ─────────►│ GET manifest.json (cert bundle)
          ◄─ current 1.5.0 / available 1.6.0, notes, size
Dashboard ─ "Install 1.6.0" (CSRF POST) ─►│ pause ADS-B + route lookups
                                          │ esp_https_ota → inactive slot
                                          │ verify signature + sha256 + size
                                          │ set boot partition, restart
LCD: UPDATING ██████░░ 62% ─────────────  │
new app boots ─► self-test ─► mark valid  │ (else bootloader rolls back)
```

Everything the device fetches comes over verified HTTPS using the same
certificate bundle the adsb.fi client uses; the image itself is additionally
signed, so a compromised host or a bad download can never be booted.

## 2. Packaging on GitHub

| Item | Where | Notes |
|---|---|---|
| Firmware image `airtrack-<ver>.bin` | GitHub **Release** asset for tag `v<ver>` | Signed with the AirTrack OTA key (ECDSA P‑256, IDF app-signature block appended). Release assets keep the git history free of binaries. |
| `airtrack-<ver>.sha256` | Release asset | Plain SHA‑256, also embedded in the manifest. |
| `docs/firmware/manifest.json` | Repo, served by GitHub Pages at `https://skitty4fingers.github.io/AirTrack/firmware/manifest.json` | Stable URL, no API rate limits, editable by hand for a rollback of the *offer* without touching binaries. |
| `docs/firmware/manifest-beta.json` | Same, optional | For units opted into a beta channel. |

Manifest schema (v1):

```json
{
  "schema": 1,
  "project": "airtrack",
  "hardware": "esp32c6-8mb",
  "channel": "stable",
  "version": "1.6.0",
  "released": "2026-09-01",
  "min_version": "1.4.0",
  "size": 1774240,
  "sha256": "e9817ec2…",
  "url": "https://github.com/Skitty4fingers/AirTrack/releases/download/v1.6.0/airtrack-1.6.0.bin",
  "notes": "Route enrichment fixes; night schedule.",
  "notes_url": "https://github.com/Skitty4fingers/AirTrack/releases/tag/v1.6.0"
}
```

`min_version` lets a release require an intermediate step (e.g. a settings
migration) and lets the manifest refuse very old firmware that predates a
protocol change. The device compares `project`/`hardware` first and ignores
anything that does not match.

### Release automation (GitHub Actions, `.github/workflows/release.yml`)

Trigger: push of tag `v*`.

1. `espressif/idf:v5.5.5` container, `idf.py build` (reproducible: pinned IDF,
   pinned managed components via `dependencies.lock`).
2. Run the existing `tools/check_release.sh` gate (host tests, transport and SD
   policy checks, size gate) — the same gate used on the bench.
3. Sign: `espsecure.py sign_data --version 2 --keyfile $OTA_SIGNING_KEY
   --output airtrack-<ver>.bin build/airtrack.bin`. The private key lives only
   in the repository secret `OTA_SIGNING_KEY`; the matching public key is
   committed as `firmware/ota_signing_key.pub` and compiled into the firmware.
4. Create the GitHub Release with the two assets and the release notes taken
   from `CHANGELOG.md`.
5. Regenerate `docs/firmware/manifest.json` and commit it to `main`
   (Pages redeploys automatically) — this is the moment the update becomes
   visible to devices, so it is the last step.

The tag version must equal `project(airtrack VERSION …)` in `CMakeLists.txt`;
the workflow fails otherwise (same check the release script already does).

## 3. Device side

### 3.1 New component `components/ota`

- `ota_check(manifest_url, result)` — fetch and parse the manifest (bounded
  4 KiB, cJSON), validate project/hardware/schema, compare semver against
  `esp_app_get_description()->version`, apply `min_version`, report
  `available`, `version`, `size`, `notes`, and a reason string when not
  applicable (`same version`, `older than installed`, `wrong hardware`,
  `needs 1.4.0 first`).
- `ota_start(url, expected_size, expected_sha256)` — runs in its own task
  (8 KiB stack): `esp_https_ota_begin` with the cert bundle, streaming
  `esp_https_ota_perform` into the inactive slot, progress counter, SHA‑256 of
  the stream, size check, then `esp_https_ota_finish` (which verifies the
  embedded signature because `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`
  is on), `esp_ota_set_boot_partition`, restart after 2 s.
- `ota_status()` — `idle | checking | available | downloading (bytes, %) |
  verifying | ready-to-restart | failed (reason) | rolled-back`.
- Failure at any point leaves the running image and boot partition untouched
  and returns to `idle` with a reason; the download can be retried.

### 3.2 Supervisor integration

- Before download: `adsb_client_set_online(false)` (frees the keep-alive TLS
  session and prevents route lookups), `storage_logger_stop()`, and hold the
  status web server open only for `/api/v1/ota/status` polling. Reference heap
  today: ~100 KiB free idle; `esp_https_ota` needs ~45 KiB peak — comfortable
  once the ADS‑B session is closed.
- LCD: a dedicated `UPDATING` screen (version, progress bar, "do not unplug"),
  then `RESTARTING`. LED: orange during download, blue after the new image is
  marked valid.
- Setup mode and BOOT-hold are ignored while an update is in flight.

### 3.3 Rollback safety

- Enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. On first boot of a new
  image the app runs a self-test — NVS settings load, LCD up, station gets an
  IPv4 lease, dashboard answers, free heap above 60 KiB — and only then calls
  `esp_ota_mark_app_valid_cancel_rollback()`. If it panics or fails the test
  within 3 minutes, the bootloader boots the previous slot on the next reset
  (`esp_ota_mark_app_invalid_rollback_and_reboot()` on test failure).
- Because this is a *bootloader* option, each unit needs **one** USB flash of
  the new bootloader together with the first OTA-capable firmware. Both units
  are on the bench now, so that is a one-time step in this release.
- Settings/NVS are never part of an update; schema migrations remain forward
  only, as today.

### 3.4 Dashboard (System card)

- **Firmware** row gains *Check for updates* → shows *Up to date (1.5.0)* or
  *1.6.0 available — notes… — [Install]*.
- Install → confirm → progress bar fed by `/api/v1/ota/status` every second →
  "Restarting… this page will reload" → the JS retries `/api/v1/status` until
  the new version answers, then shows *Updated to 1.6.0*.
- Options (Display/System): **Update channel** (stable/beta) and **Check
  automatically** (daily check; *notify only* by default — a badge on the
  Firmware row and a small dot on the LCD header; an opt-in **install
  automatically at night** uses the existing night window).
- **Install from file** fallback: upload a signed `.bin` from the browser
  (`POST /api/v1/ota/upload`, chunked, same verification path) for units
  without internet reachability to GitHub.

### 3.5 API

| Method/path | Purpose |
|---|---|
| `GET /api/v1/ota/check` | Fetch manifest now; returns comparison result |
| `GET /api/v1/ota/status` | Current OTA state and progress |
| `POST /api/v1/ota/start` | CSRF; body `version=` must match the last check |
| `POST /api/v1/ota/upload` | CSRF; raw signed image body, bounded to slot size |
| `GET /api/v1/status` | Adds `firmware`, `ota_partition`, `pending_verify` |

All mutating calls keep the existing rules: canonical Host, CSRF token from
the page, form/octet-stream content types, bounded bodies.

## 4. Security properties

- Transport: HTTPS with the ESP certificate bundle for manifest and image;
  redirects (GitHub release assets redirect once to
  `objects.githubusercontent.com`) are followed only to `https://` hosts under
  `github.com` / `githubusercontent.com` / `github.io`.
- Authenticity: image signature verified before the boot partition changes;
  the public key is compiled in, the private key never leaves GitHub secrets
  (or the maintainer's machine for a manual release).
- Integrity: manifest `sha256` and `size` must match the streamed image.
- Downgrade: only versions greater than the running one are offered;
  the file-upload path allows an explicit downgrade with a confirmation.
- Availability: a failed or interrupted download never touches the running
  slot; rollback protects against a bad image that boots but misbehaves.

## 5. Delivery

1. **Firmware** (this repo, one release): `components/ota`, supervisor and
   LCD integration, System-card UI, signing key + `sdkconfig.defaults`
   (`SECURE_SIGNED_APPS_ECDSA_V2_SCHEME`, `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`,
   `BOOTLOADER_APP_ROLLBACK_ENABLE`), bootloader reflash of both bench units,
   host tests for semver/manifest parsing.
2. **Repository**: `.github/workflows/release.yml`, `docs/firmware/manifest.json`,
   `CHANGELOG.md`, checklist updates; first tagged release `v1.6.0` produced by
   the workflow and installed on one bench unit *via the dashboard* as the
   acceptance test (the other unit stays on USB-flashed firmware as control).
3. **Later**: beta channel, auto-install at night, and a "factory image"
   asset for USB recovery.

## 6. Open decisions

- Manifest on GitHub Pages (proposed) vs. GitHub Releases API: Pages is
  simpler and rate-limit free; the API would remove the manifest commit step.
- Automatic checks default: proposed *on, notify only*.
- Whether to keep a fully manual release path (bench build + `espsecure.py`
  + `gh release create`) alongside Actions — proposed yes, documented in
  `RELEASE_CHECKLIST.md`.
