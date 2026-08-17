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

"${output_dir}/test_tracker"
