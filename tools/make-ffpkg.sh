#!/usr/bin/env bash
# PSCord - Packs the built app folder into a ShadowMount .ffpkg (UFS2) image.
# Copyright (C) 2026
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs on the host (macOS or Linux) after `make`: needs dotnet 8+ and git.
# UFS2Tool is fetched at a pinned revision into the ignored .deps/ cache.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
for command in dotnet git python3; do
    command -v "$command" >/dev/null || {
        echo "missing required command: $command" >&2
        exit 2
    }
done
dotnet_major=$(dotnet --version | cut -d. -f1)
[[ $dotnet_major =~ ^[0-9]+$ && $dotnet_major -ge 8 ]] || {
    echo "UFS2Tool requires the .NET SDK 8 or newer" >&2
    exit 2
}

title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
    "$root/sce_sys/param.json")
dist="$root/dist"
app="$dist/$title_id"
[[ -f $app/eboot.bin && -f $app/payload/bg-voice.elf && -f $app/payload/party-audio-pump.elf ]] || {
    echo "build the app folder first (make): $app is incomplete" >&2
    exit 2
}

checkout="$root/.deps/UFS2Tool"
git_checkout=(git -c "safe.directory=$checkout" -C "$checkout")
revision="b5307a60d5b4e3a68ba680e0e33cfadf05017c77"
if [[ ! -d $checkout/.git ]]; then
    mkdir -p "$checkout"
    "${git_checkout[@]}" init --quiet
    "${git_checkout[@]}" remote add origin https://github.com/SvenGDK/UFS2Tool.git
    "${git_checkout[@]}" fetch --quiet --depth 1 origin "$revision"
    "${git_checkout[@]}" checkout --quiet --detach FETCH_HEAD
fi
[[ $("${git_checkout[@]}" rev-parse HEAD) == "$revision" ]] || {
    echo "UFS2Tool cache is not at $revision" >&2
    exit 2
}
output="$checkout/.build-host"
binary="$output/UFS2Tool.dll"
stamp="$output/.revision"
if [[ ! -f $binary || ! -f $stamp || $(<"$stamp") != "$revision" ]]; then
    dotnet build "$checkout/UFS2Tool.csproj" --configuration Release \
        --output "$output" --nologo --verbosity quiet >&2
    printf '%s\n' "$revision" > "$stamp"
fi

# The console only starts an app whose files are open to all.
chmod -R a+rwX "$app"
image="$dist/$title_id.ffpkg"
rm -f -- "$image"
DOTNET_ROLL_FORWARD=Major dotnet "$binary" makefs -S 4096 -b 20% -t ffs \
    -o version=2,bsize=32768,fsize=4096,minfree=0,softupdates=0,optimization=space \
    "$image" "$app"
python3 - "$image" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as stream:
    stream.seek(0x1055c)
    if struct.unpack("<I", stream.read(4))[0] != 0x19540119:
        raise SystemExit("FFPKG is missing the UFS2 superblock magic")
PY
(cd "$dist" && shasum -a 256 "$title_id.ffpkg" > "$title_id.ffpkg.sha256")
printf 'FFPKG: %s\n' "$image"
