#!/usr/bin/env bash
# Publish an AirTrack firmware release for over-the-air updates.
#
#   GITHUB_TOKEN=... tools/publish_release.sh 1.6.0 "Release notes"
#
# 1. Verifies the version matches CMakeLists.txt.
# 2. Runs tools/check_release.sh (host tests, policy checks, size gate).
# 3. Creates GitHub Release v<version> and uploads airtrack-<version>.bin.
# 4. Writes docs/firmware/manifest.json, commits, and pushes (GitHub Pages
#    serves it at https://skitty4fingers.github.io/AirTrack/firmware/manifest.json).
#
# Publishing the manifest is the step that makes the update visible to
# devices; delete or edit that file to withdraw an offer.
set -euo pipefail

version="${1:-}"
notes="${2:-AirTrack ${version}}"
repo="Skitty4fingers/AirTrack"
project_dir="$(cd "$(dirname "$0")/.." && pwd)"

if [[ -z "$version" ]]; then
    echo "usage: GITHUB_TOKEN=... $0 <version> [notes]" >&2
    exit 1
fi
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

"${project_dir}/tools/check_release.sh"

image="${project_dir}/build-production/airtrack.bin"
asset="airtrack-${version}.bin"
size="$(stat -c '%s' "$image")"
sha256="$(sha256sum "$image" | cut -d' ' -f1)"
api="https://api.github.com/repos/${repo}"
auth=(-H "Authorization: Bearer ${GITHUB_TOKEN}" -H "Accept: application/vnd.github+json")

echo "publish: ${asset} (${size} bytes, ${sha256})"

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

python3 - "$version" "$size" "$sha256" "$download_url" "$notes" "${project_dir}/docs/firmware/manifest.json" <<'PY'
import json, sys, datetime
version, size, sha256, url, notes, path = sys.argv[1:7]
manifest = {
    "version": version,
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

git -C "$project_dir" add docs/firmware/manifest.json
git -C "$project_dir" -c user.name="skitty" -c user.email="scottsdamgaard@gmail.com" \
    commit -q -m "Publish firmware ${version} (OTA manifest)"
git -C "$project_dir" -c credential.helper='!f() { echo username=Skitty4fingers; echo "password=$GITHUB_TOKEN"; }; f' push
echo "publish: manifest live shortly at https://skitty4fingers.github.io/AirTrack/firmware/manifest.json"
