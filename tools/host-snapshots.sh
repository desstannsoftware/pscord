#!/usr/bin/env bash
# ps5-homebrew-ui - Render every design on the host (Mesa surfaceless EGL) to PNG.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/host-snapshots.sh [output dir] [design id|all] [width height]
#
# Builds the platform-neutral part of the app for the PC and runs its tour
# (see src/app/tour.hpp). HUI_STRIP=<from>,<to> writes the frames of the
# switch between two designs instead.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/ninja-build.sh"
cxx=$(command -v "${HOST_CXX:-clang++}")
cc=$(command -v "${HOST_CC:-clang}")
build="$root/build/host-snapshots"
ninja_begin "$build/build.ninja"

# Everything under src/ except the console platform layer and entry point.
sources=("$root/host/snapshot_main.cpp" "$root/host/manifest.cpp" "$root/host/platform_host.cpp"
    "$root/host/dc_preview.cpp" "$root/src/dc/net/http_server.cpp" "$root/src/dc/markdown.cpp"
    "$root/src/dc/media_decode.cpp")
# src/pkg needs zlib: the native dependency tree ships its header, the host its library.
zlib_include="$root/.deps/native/zlib/root/usr/include"
zlib_lib=$(ls /usr/lib/x86_64-linux-gnu/libz.so* /usr/lib/libz.so* 2>/dev/null | head -1 || true)
while IFS= read -r -d '' source; do
    sources+=("$source")
done < <(find "$root/src" -type f \( -name '*.cpp' -o -name '*.c' \) \
    ! -path "$root/src/platform/*" ! -path "$root/src/runtime/*" ! -path "$root/src/dc/*" \
    ! -path "$root/src/voice/*" ! -path "$root/src/update/*" \
    ! -name main.cpp -print0 | sort -z)

objects=()
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    if [[ $source == *.c ]]; then
        ninja_inputs=("$source" "$cc")
        ninja_edge CC "$object" "${compiler_cache[@]}" "$cc" -std=c11 -O2 -w -I"$root/src" \
            -DECB=1 -DAES128=1 -MD -MF "$object.d" -c "$source" -o "$object"
    else
        ninja_inputs=("$source" "$cxx")
        ninja_edge CXX "$object" "${compiler_cache[@]}" "$cxx" -std=c++20 -O2 -Wall -Wextra \
            -DGL_GLEXT_PROTOTYPES=1 -I"$root/src" -I"$root/host" -I"$root/third_party" -I"$zlib_include" \
            -MD -MF "$object.d" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
ninja_inputs=("${objects[@]}")
ninja_edge LINK "$build/hui_snapshots" "$cxx" "${objects[@]}" ${zlib_lib:+"$zlib_lib"} -lEGL -lGL -lm -lpthread \
    -o "$build/hui_snapshots"
if ! ninja_run >"$build/build.log" 2>&1; then
    grep -E 'error|FAILED|warning' -A6 "$build/build.log" | head -80 >&2
    exit 1
fi
grep -E 'warning' -A4 "$build/build.log" | head -40 >&2 || true

output=${1:-"$root/build/snapshots"}
mkdir -p "$output"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    "$build/hui_snapshots" "$root/assets" "$output" "${@:2}"
