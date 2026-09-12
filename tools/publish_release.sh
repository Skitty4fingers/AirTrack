#!/usr/bin/env bash
# Publish an AirTrack firmware release for over-the-air updates.
#
#   GITHUB_TOKEN=... tools/publish_release.sh [--board <id>] 1.6.0 "Release notes"
#
# Boards: esp32c6-lcd-1.47 (default) and esp32c6-touch-lcd-2.8.  Each board is
# published independently: its own release asset, its own Pages manifest, and
# its own merged factory image.  The manifest carries a "board" field and the
# firmware refuses one that names different hardware, so a device can never
# install an image that would leave its panel dark.
#
# 1. Verifies the version matches CMakeLists.txt.
# 2. Runs tools/check_release.sh (host tests, policy checks, size gate).
# 3. Creates GitHub Release v<version> and uploads airtrack-<version>.bin.
# 4. Writes docs/firmware/manifest.json, commits, and pushes (GitHub Pages
#    serves it at https://skitty4fingers.github.io/AirTrack/firmware/manifest.json).
# 5. Merges bootloader + partition table + otadata + app into
#    docs/firmware/airtrack-<board>-<version>-factory.bin and points
#    docs/firmware/web-flash.json at it; that is what docs/flash.html writes
#    over USB from the browser. The image has to live on the Pages origin:
#    GitHub release assets are served without CORS headers, so a browser
#    cannot fetch them cross-origin.
#
# Publishing the manifest is the step that makes the update visible to
# devices; delete or edit that file to withdraw an offer.
set -euo pipefail

board="esp32c6-lcd-1.47"
while [[ "${1:-}" == --board* ]]; do
    case "$1" in
        --board) board="${2:-}"; shift 2 ;;
        --board=*) board="${1#--board=}"; shift ;;
    esac
done

version="${1:-}"
notes="${2:-AirTrack ${version}}"
repo="Skitty4fingers/AirTrack"
project_dir="$(cd "$(dirname "$0")/.." && pwd)"

if [[ -z "$version" ]]; then
    echo "usage: GITHUB_TOKEN=... $0 [--board <id>] <version> [notes]" >&2
    exit 1
fi

# Per-board publishing targets.  The 1.47 keeps the historical asset and
# manifest names because units already in the field poll them.
case "$board" in
    esp32c6-lcd-1.47)
        build_dir="${project_dir}/build-production"
        asset="airtrack-${version}.bin"
        manifest_name="manifest.json"
        web_flash_name="web-flash.json"
        factory="airtrack-${version}-factory.bin"
        flash_size="8MB"
        ;;
    esp32c6-touch-lcd-2.8)
        build_dir="${project_dir}/build-production-touch28"
        asset="airtrack-${board}-${version}.bin"
        manifest_name="manifest-${board}.json"
        web_flash_name="web-flash-${board}.json"
        factory="airtrack-${board}-${version}-factory.bin"
        flash_size="16MB"
        ;;
    *)
        echo "unknown board '${board}'" >&2
        exit 1
        ;;
esac
if [[ -z "${GITHUB_TOKEN:-}" ]]; then
    echo "GITHUB_TOKEN is not set" >&2
    exit 1
fi
if ! grep -q "project(airtrack VERSION ${version//./\\.})" "${project_dir}/CMakeLists.txt"; then
    echo "CMakeLists.txt does not declare VERSION ${version}" >&2
    exit 1
fi
if [[ -n "$(git -C "$project_dir" status --porcelain)" ]]; then
    echo "working tree is not clean; commit first" >&2
    exit 1
fi

"${project_dir}/tools/check_release.sh" --board "$board"

image="${build_dir}/airtrack.bin"
size="$(stat -c '%s' "$image")"
sha256="$(sha256sum "$image" | cut -d' ' -f1)"
api="https://api.github.com/repos/${repo}"
auth=(-H "Authorization: Bearer ${GITHUB_TOKEN}" -H "Accept: application/vnd.github+json")

echo "publish: ${board} ${asset} (${size} bytes, ${sha256})"

release_json="$(curl -sS "${auth[@]}" "${api}/releases/tags/v${version}" || true)"
release_id="$(python3 -c 'import json,sys; j=json.load(sys.stdin); print(j.get("id",""))' <<<"$release_json")"
if [[ -z "$release_id" ]]; then
    payload="$(python3 -c 'import json,sys; print(json.dumps({"tag_name":"v"+sys.argv[1],"target_commitish":"main","name":"AirTrack "+sys.argv[1],"body":sys.argv[2],"draft":False,"prerelease":False}))' "$version" "$notes")"
    release_json="$(curl -sS "${auth[@]}" -X POST "${api}/releases" -d "$payload")"
    release_id="$(python3 -c 'import json,sys; j=json.load(sys.stdin); print(j["id"])' <<<"$release_json")"
    echo "publish: created release v${version} (id ${release_id})"
else
    echo "publish: release v${version} exists (id ${release_id}); replacing asset"
    asset_id="$(python3 -c 'import json,sys; j=json.load(sys.stdin); print(next((a["id"] for a in j.get("assets",[]) if a["name"]==sys.argv[1]),""))' "$asset" <<<"$release_json")"
    if [[ -n "$asset_id" ]]; then
        curl -sS "${auth[@]}" -X DELETE "${api}/releases/assets/${asset_id}" >/dev/null
    fi
fi

upload_json="$(curl -sS "${auth[@]}" -H "Content-Type: application/octet-stream" \
    --data-binary @"$image" \
    "https://uploads.github.com/repos/${repo}/releases/${release_id}/assets?name=${asset}")"
download_url="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["browser_download_url"])' <<<"$upload_json")"
echo "publish: uploaded ${download_url}"

# Factory image for the browser installer: one blob written at offset 0.
factory_path="${project_dir}/docs/firmware/${factory}"
python3 -m esptool --chip esp32c6 merge_bin -o "$factory_path" \
    --flash_mode dio --flash_size "$flash_size" --flash_freq 80m \
    0x0 "${build_dir}/bootloader/bootloader.bin" \
    0x8000 "${build_dir}/partition_table/partition-table.bin" \
    0xf000 "${build_dir}/ota_data_initial.bin" \
    0x20000 "$image" >/dev/null
factory_size="$(stat -c '%s' "$factory_path")"
factory_sha="$(sha256sum "$factory_path" | cut -d' ' -f1)"
echo "publish: ${factory} (${factory_size} bytes, ${factory_sha})"

# Keep only this board's current factory image on Pages; older ones stay in
# the tags, and the other board's current image is left alone.
case "$board" in
    esp32c6-lcd-1.47)
        # Historical naming carries no board segment, so match a version digit
        # rather than a wildcard that would also catch another board's file.
        factory_glob="${project_dir}/docs/firmware/airtrack-[0-9]*-factory.bin"
        ;;
    *)
        factory_glob="${project_dir}/docs/firmware/airtrack-${board}-*-factory.bin"
        ;;
esac
for old_factory in $factory_glob; do
    if [[ -f "$old_factory" && "$old_factory" != "$factory_path" ]]; then
        git -C "$project_dir" rm -q --ignore-unmatch "$old_factory" || rm -f "$old_factory"
    fi
done

python3 - "$version" "$factory" "$factory_size" "$factory_sha" "${project_dir}/docs/firmware/${web_flash_name}" "$board" <<'PY'
import json, sys
version, factory, size, sha256, path, board = sys.argv[1:7]
manifest = {
    "name": "AirTrack (%s)" % board,
    "version": version,
    "new_install_prompt_erase": True,
    "new_install_improv_wait_time": 0,
    "sha256": sha256,
    "size": int(size),
    "builds": [{
        "chipFamily": "ESP32-C6",
        "parts": [{"path": factory, "offset": 0}],
    }],
}
with open(path, "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY

python3 - "$version" "$size" "$sha256" "$download_url" "$notes" "${project_dir}/docs/firmware/${manifest_name}" "$board" <<'PY'
import json, sys, datetime
version, size, sha256, url, notes, path, board = sys.argv[1:8]
manifest = {
    "version": version,
    # Devices refuse a manifest that names hardware other than their own.
    "board": board,
    "size": int(size),
    "sha256": sha256,
    "url": url,
    "notes": notes,
    "released": datetime.date.today().isoformat(),
}
with open(path, "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY

git -C "$project_dir" add -A docs/firmware
git -C "$project_dir" -c user.name="skitty" -c user.email="scottsdamgaard@gmail.com" \
    commit -q -m "Publish firmware ${version} for ${board} (OTA manifest and browser-flash image)"
git -C "$project_dir" -c credential.helper='!f() { echo username=Skitty4fingers; echo "password=$GITHUB_TOKEN"; }; f' push
echo "publish: manifest live shortly at https://skitty4fingers.github.io/AirTrack/firmware/${manifest_name}"
echo "publish: browser installer at https://skitty4fingers.github.io/AirTrack/flash.html"
