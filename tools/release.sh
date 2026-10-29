#!/usr/bin/env bash
# ps5-discord - Builds a release and its OTA update; --publish uploads it.
#   tools/release.sh 01.001.000 "Sürüm notu" [--publish]
# Sets contentVersion in sce_sys/param.json, builds in the container, writes
# dist/release/<version>/PPSA99190.zip.
# --publish uploads it to desstannsoftware.com; every installed PSCord then
# updates itself on its next launch, and the store offers the new version.
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
publish_sh=${DESSTANN_PUBLISH:-$root/../../../pkg-folderizer/telemetry/tools/publish.sh}

version=${1:-}
notes=${2:-}
publish=${3:-}
[[ $version =~ ^[0-9]{2}\.[0-9]{3}\.[0-9]{3}$ ]] || {
    echo "usage: tools/release.sh NN.NNN.NNN \"notes\" [--publish]" >&2; exit 2;
}
[[ -z $publish || $publish == --publish ]] || { echo "unknown option: $publish" >&2; exit 2; }

current=$(sed -nE 's/.*"contentVersion": *"([0-9.]+)".*/\1/p' "$root/sce_sys/param.json")
[[ $version > $current ]] || { echo "version $version is not newer than $current" >&2; exit 2; }
sed -i '' -E "s/(\"contentVersion\": *\")[0-9.]+\"/\1$version\"/" "$root/sce_sys/param.json"

bash "$root/docker/run.sh" make
out="$root/dist/release/$version"
rm -rf -- "$out"
mkdir -p "$out"
(cd "$root/dist" && zip -qr "$out/PPSA99190.zip" PPSA99190)
printf -- '- %s\n' "$notes" > "$out/notes.md"

if [[ $publish == --publish ]]; then
    bash "$publish_sh" --optional pscord "$version" "$out/notes.md" "$out/PPSA99190.zip"
else
    echo "built $out (run again with --publish to release it)"
fi
