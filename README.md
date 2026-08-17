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

The connected unit is running the AirTrack 1.0.0 production candidate. The
implemented core includes:

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

The current image is 1,529,536 bytes and leaves 62 percent of each 3,904 KiB
OTA app slot free. The partition table reserves dual OTA slots, but signed web
OTA and rollback are intentionally deferred; update 1.0.0 over native USB.

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

See [the firmware plan](docs/FIRMWARE_PLAN.md) for the architecture and
[the release checklist](docs/RELEASE_CHECKLIST.md) for target acceptance and
the remaining production sign-off tests.
