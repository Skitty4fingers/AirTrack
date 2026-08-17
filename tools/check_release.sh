#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
idf_dir="${IDF_PATH:-/home/skitty/esp/esp-idf-v5.5.5}"
build_dir="${project_dir}/build-production"
size_limit_bytes=$((3584 * 1024))

if ! grep -q 'project(airtrack VERSION 1\.2\.0)' \
    "${project_dir}/CMakeLists.txt"; then
    echo "release check: project version is not 1.2.0" >&2
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

"${project_dir}/tools/run_host_tests.sh"

source "${idf_dir}/export.sh" >/dev/null
idf.py -B "${build_dir}" build

image="${build_dir}/airtrack.bin"
image_size="$(stat -c '%s' "${image}")"
if (( image_size > size_limit_bytes )); then
    echo "release check: ${image_size} byte image exceeds ${size_limit_bytes} byte gate" >&2
    exit 1
fi

echo "release check: PASS (${image_size} bytes)"
sha256sum "${image}" \
    "${build_dir}/bootloader/bootloader.bin" \
    "${build_dir}/partition_table/partition-table.bin"
