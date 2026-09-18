#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
idf_dir="${IDF_PATH:-/home/skitty/esp/esp-idf-v5.5.5}"
output_dir="${project_dir}/build-host-tests"
mkdir -p "${output_dir}"

cc -std=gnu17 -O2 -Wall -Wextra -Werror \
  -I"${project_dir}/components/tracker/include" \
  -I"${project_dir}/components/config/include" \
  -I"${project_dir}/build/config" \
  -I"${idf_dir}/components/esp_common/include" \
  -I"${idf_dir}/components/json/cJSON" \
  "${project_dir}/components/tracker/airtrack_tracker.c" \
  "${idf_dir}/components/json/cJSON/cJSON.c" \
  "${project_dir}/test/host/test_tracker.c" \
  -lm -o "${output_dir}/test_tracker"

# Flight details: identity lookups, phase, response parsers, PNG decoding.
# The device inflates with the ROM's tinfl; the test supplies a stored-block
# inflate, so flight_png_inflate.c is not built here.
cc -std=gnu17 -O2 -Wall -Wextra -Werror \
  -I"${project_dir}/components/tracker/include" \
  -I"${project_dir}/components/config/include" \
  -I"${project_dir}/components/flight_info/include" \
  -I"${project_dir}/build/config" \
  -I"${idf_dir}/components/esp_common/include" \
  -I"${idf_dir}/components/json/cJSON" \
  "${project_dir}/components/tracker/airtrack_tracker.c" \
  "${project_dir}/components/flight_info/flight_info_parse.c" \
  "${project_dir}/components/flight_info/flight_png.c" \
  "${idf_dir}/components/json/cJSON/cJSON.c" \
  "${project_dir}/test/host/test_flight.c" \
  -lm -o "${output_dir}/test_flight"

"${output_dir}/test_tracker"
"${output_dir}/test_flight"
