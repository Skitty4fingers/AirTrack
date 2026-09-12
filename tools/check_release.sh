#!/usr/bin/env bash
#
# Reproducible release gate for one board.
#
#   tools/check_release.sh [--board <id>]
#
# Boards:
#   esp32c6-lcd-1.47       (default) Waveshare ESP32-C6-LCD-1.47, 8 MB
#   esp32c6-touch-lcd-2.8            Waveshare ESP32-C6-Touch-LCD-2.8, 16 MB
#
# Each board builds into its own directory so the two images never overwrite
# each other, and prints the hashes the release checklist records.
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
idf_dir="${IDF_PATH:-/home/skitty/esp/esp-idf-v5.5.5}"
size_limit_bytes=$((3584 * 1024))
board="esp32c6-lcd-1.47"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --board)
            board="${2:-}"
            shift 2
            ;;
        --board=*)
            board="${1#--board=}"
            shift
            ;;
        *)
            echo "usage: $0 [--board esp32c6-lcd-1.47|esp32c6-touch-lcd-2.8]" >&2
            exit 1
            ;;
    esac
done

case "$board" in
    esp32c6-lcd-1.47)
        build_dir="${project_dir}/build-production"
        sdkconfig_defaults="sdkconfig.defaults"
        ;;
    esp32c6-touch-lcd-2.8)
        build_dir="${project_dir}/build-production-touch28"
        sdkconfig_defaults="sdkconfig.defaults;sdkconfig.defaults.${board}"
        ;;
    *)
        echo "release check: unknown board '${board}'" >&2
        exit 1
        ;;
esac

if ! grep -q 'project(airtrack VERSION 1\.7\.0)' \
    "${project_dir}/CMakeLists.txt"; then
    echo "release check: project version is not 1.7.0" >&2
    exit 1
fi

if grep -R -n -E \
    'skip_cert_common_name_check[[:space:]]*=[[:space:]]*true|transport_type[[:space:]]*=[[:space:]]*HTTP_TRANSPORT_OVER_TCP' \
    "${project_dir}/components/adsb_client"; then
    echo "release check: insecure ADS-B transport setting found" >&2
    exit 1
fi

if ! grep -q 'format_if_mount_failed = false' \
    "${project_dir}/components/board/board_sd.c"; then
    echo "release check: SD non-formatting policy is missing" >&2
    exit 1
fi

# An image that could accept a manifest for other hardware would install it
# and leave the panel dark, so the board check has to stay in the OTA path.
if ! grep -q 'strcmp(board, BOARD_ID) != 0' \
    "${project_dir}/components/ota/ota_update.c"; then
    echo "release check: OTA board-identifier check is missing" >&2
    exit 1
fi

"${project_dir}/tools/run_host_tests.sh"

source "${idf_dir}/export.sh" >/dev/null
# SDKCONFIG must live inside the build directory.  idf.py otherwise keeps one
# sdkconfig at the project root and reuses it across build directories, so
# building a second board would silently inherit the first board's settings.
idf.py -B "${build_dir}"     -D SDKCONFIG_DEFAULTS="${sdkconfig_defaults}"     -D SDKCONFIG="${build_dir}/sdkconfig"     build

# The generated config must actually name the board that was asked for, so a
# stale build directory cannot be published under the wrong board identifier.
case "$board" in
    esp32c6-lcd-1.47) expected_symbol="CONFIG_AIRTRACK_BOARD_LCD_1_47" ;;
    esp32c6-touch-lcd-2.8) expected_symbol="CONFIG_AIRTRACK_BOARD_TOUCH_LCD_2_8" ;;
esac
if ! grep -q "^#define ${expected_symbol} 1$" "${build_dir}/config/sdkconfig.h"; then
    echo "release check: ${build_dir} is not configured for ${board}" >&2
    exit 1
fi

image="${build_dir}/airtrack.bin"
image_size="$(stat -c '%s' "${image}")"
if (( image_size > size_limit_bytes )); then
    echo "release check: ${image_size} byte image exceeds ${size_limit_bytes} byte gate" >&2
    exit 1
fi

echo "release check: PASS for ${board} (${image_size} bytes)"
sha256sum "${image}" \
    "${build_dir}/bootloader/bootloader.bin" \
    "${build_dir}/partition_table/partition-table.bin"
