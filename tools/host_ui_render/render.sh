#!/usr/bin/env bash
# Render the AirTrack LCD screens on the host for layout review.
# Usage: tools/host_ui_render/render.sh [output-dir]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
project="$(cd "${here}/../.." && pwd)"
idf_dir="${IDF_PATH:-/home/skitty/esp/esp-idf-v5.5.5}"
lvgl="${project}/managed_components/lvgl__lvgl"
qr="${project}/managed_components/espressif__qrcode"
out="${1:-${project}/build-host-ui}"
obj="${out}/obj"
mkdir -p "${obj}"

cflags=(-std=gnu17 -O1 -w
  -DLV_CONF_KCONFIG_EXTERNAL_INCLUDE='"sdkconfig.h"' -DLV_CONF_SKIP
  -I"${project}/build/config" -I"${lvgl}" -I"${lvgl}/src"
  -I"${here}/stubs" -I"${idf_dir}/components/esp_common/include"
  -I"${project}/components/ui/include" -I"${project}/components/config/include"
  -I"${project}/components/tracker/include" -I"${qr}/include")

# LVGL core (skip optional driver/lib trees; all are disabled by Kconfig anyway).
mapfile -t sources < <(find "${lvgl}/src" -name '*.c' \
  -not -path '*/drivers/*' -not -path '*/others/*' | sort)
if [ ! -f "${obj}/liblvgl_host.a" ]; then
  printf '%s\n' "${sources[@]}" | xargs -P"$(nproc)" -I{} sh -c \
    'cc '"$(printf '%q ' "${cflags[@]}")"' -c "$1" -o "'"${obj}"'/$(echo "$1" | md5sum | cut -c1-16).o"' _ {}
  ar rcs "${obj}/liblvgl_host.a" "${obj}"/*.o
fi

cc "${cflags[@]}" -o "${out}/render_ui" "${here}/render_ui.c" \
  "${qr}/esp_qrcode_main.c" "${qr}/esp_qrcode_wrapper.c" "${qr}/qrcodegen.c" \
  "${project}/components/tracker/airtrack_tracker.c" \
  "${idf_dir}/components/json/cJSON/cJSON.c" -I"${idf_dir}/components/json/cJSON" \
  "${obj}/liblvgl_host.a" -lm
"${out}/render_ui" "${out}"
python3 "${here}/ppm2png.py" "${out}"/*.ppm
echo "screens: ${out}/*.png"
