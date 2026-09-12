# Supported boards

AirTrack is board-specific: the pin map, panel geometry, backlight path, and
available peripherals differ between targets. One Kconfig choice selects the
board, and everything that varies is reached through `components/board`.

| | ESP32-C6-LCD-1.47 | ESP32-C6-Touch-LCD-2.8 |
|---|---|---|
| Board identifier | `esp32c6-lcd-1.47` | `esp32c6-touch-lcd-2.8` |
| Kconfig symbol | `AIRTRACK_BOARD_LCD_1_47` | `AIRTRACK_BOARD_TOUCH_LCD_2_8` |
| Module | ESP32-C6FH8 | ESP32-C6-WROOM-1 |
| Flash | 8 MB | 16 MB |
| Partition table | `partitions.csv` (3,904 KiB slots) | `partitions-16mb.csv` (6 MiB slots) |
| Panel | ST7789, 172 x 320 | ST7789, 240 x 320 |
| X RAM offset | 34 px | none |
| Panel clock | 12 MHz | 40 MHz |
| Backlight | GPIO22, LEDC PWM | CH32 expander PWM register |
| Panel reset | GPIO21 | CH32 expander EXIO1 |
| Status LED | WS2812B on GPIO8 | none |
| Extra sensors | none | PCF85063A RTC, SHTC3 |
| Touch | none | CST3530, **not used** |

Both panels are 320 pixels tall, which is why one UI serves both: the screens
are laid out against a 172-pixel design width, full-width elements derive from
`BOARD_LCD_H_RES`, and the handful of absolutely positioned blocks are shifted
by `UI_WIDE_PAD` so they stay centred on a wider panel. That constant is
exactly zero at 172 px, so the 1.47 rendering is untouched.

## Pin maps

### Waveshare ESP32-C6-LCD-1.47

| Function | Pin |
|---|---|
| LCD MOSI / SCLK | GPIO6 / GPIO7 |
| LCD CS / DC / reset | GPIO14 / GPIO15 / GPIO21 |
| LCD backlight | GPIO22 (active-high LEDC PWM) |
| SD CS / MISO | GPIO4 / GPIO5 |
| WS2812B | GPIO8 |
| BOOT button | GPIO9 (active low) |

### Waveshare ESP32-C6-Touch-LCD-2.8

| Function | Pin |
|---|---|
| LCD MOSI / SCLK | GPIO1 / GPIO0 |
| LCD CS / DC | GPIO11 / GPIO10 |
| LCD reset | CH32 expander EXIO1 |
| LCD backlight | CH32 expander PWM register |
| SD CS / MISO | GPIO23 / GPIO8 |
| Shared I2C SDA / SCL | GPIO6 / GPIO7 at 400 kHz |
| BOOT button | GPIO9 (active low) |

Note that GPIO6 and GPIO7 swap roles between the two boards: SPI data and
clock on the 1.47, the I2C bus on the 2.8.

## The CH32V003 I/O expander

The 2.8 puts three signals behind a CH32V003 co-processor at I2C address
`0x24` instead of ESP32 GPIO, which is the single biggest difference between
the boards. `components/board/board_exio.c` drives it.

| Register | Purpose |
|---|---|
| `0x02` | Pin mode; AirTrack writes `0xFF` (all outputs) |
| `0x03` | Output levels, one bit per channel |
| `0x04` | Input levels |
| `0x05` | Backlight PWM duty, 0-255 |
| `0x06` | ADC, 16-bit little-endian |

| Channel | Signal |
|---|---|
| EXIO0 | Touch panel reset (held released) |
| EXIO1 | LCD reset |
| EXIO3 | Audio amplifier enable |

The output register is write-only in practice, so the board component keeps a
shadow copy (`board_state_t::exio_outputs`) and rewrites the whole byte on
every change.

Two consequences follow from the backlight being an I2C register:

- Brightness changes cost a bus transaction rather than a PWM update. The
  supervisor already only writes when the computed level changes, so the night
  schedule and the dashboard slider stay cheap.
- The bus must come up before the panel. `board_init()` therefore treats an
  I2C failure on this board as fatal, unlike the SD card or the status LED,
  which stay optional.

The panel reset is pulsed explicitly over I2C before `esp_lcd_panel_reset()`
issues its soft reset, because esp_lcd owns no reset pin here.

## Building for a board

The base `sdkconfig.defaults` targets the 1.47. Every other board layers its
own file on top:

```sh
# 1.47 (default)
idf.py -B build-production build

# Touch LCD 2.8
idf.py -B build-production-touch28 \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6-touch-lcd-2.8" \
    build
```

`tools/check_release.sh --board <id>` does this for you and additionally
verifies that the generated config really names the board it was asked for, so
a stale build directory cannot be published under the wrong identifier.

## Updates are per board

Each board publishes its own release asset, its own Pages manifest, and its
own merged factory image:

| | 1.47 | Touch 2.8 |
|---|---|---|
| OTA manifest | `docs/firmware/manifest.json` | `docs/firmware/manifest-esp32c6-touch-lcd-2.8.json` |
| Installer manifest | `docs/firmware/web-flash.json` | `docs/firmware/web-flash-esp32c6-touch-lcd-2.8.json` |

The 1.47 keeps the historical unsuffixed names because units already in the
field poll them.

Two independent checks keep a device from installing the wrong image, which
would flash cleanly and then leave the panel dark:

1. The manifest URL is compiled in, per board.
2. The manifest carries a `board` field, and the firmware refuses one that
   names different hardware before the version is ever offered.

Manifests published before board identifiers existed carry no `board` key.
Only the 1.47 still honours those, since its devices are the ones already
polling that URL; every other board requires an explicit match.

## Adding another board

1. Add a `config AIRTRACK_BOARD_*` entry to
   `components/board/Kconfig.projbuild`.
2. Add the geometry, capability flags, `BOARD_ID`, and `BOARD_NAME` block to
   `components/board/include/board.h`, and the pin map to
   `components/board/board_profile.h`.
3. Add the vendor ST7789 init table to `components/board/board_lcd.c`.
4. Add a `sdkconfig.defaults.<board-id>` with the flash size, partition table,
   and board symbol, plus a partition CSV if the flash size is new.
5. Add the board to the `case` blocks in `tools/check_release.sh` and
   `tools/publish_release.sh`, and to the `BOARDS` table in `docs/flash.html`.

The capability flags (`BOARD_HAS_RGB_LED`, `BOARD_HAS_I2C_BUS`,
`BOARD_HAS_IO_EXPANDER`, `BOARD_HAS_RTC`, `BOARD_HAS_ENVIRONMENT_SENSOR`) are
what keep the rest of the tree free of board conditionals: absent hardware
returns `ESP_ERR_NOT_SUPPORTED`, and every caller already treats these as
best-effort.
