#!/usr/bin/env bash
# ps5-discord - Build the background voice daemon (payload ELF).
# Copyright (C) 2026 Desstann
# SPDX-License-Identifier: MIT
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$root/build/bg-voice.elf"}
sdk=${PS5_PAYLOAD_SDK:-"$root/.deps/native/ps5-payload-sdk"}
pacbrew=$(echo "$root"/.deps/pacbrew/*/sysroot/user/homebrew)
cxx="$sdk/bin/prospero-clang++"
cc="$sdk/bin/prospero-clang"
dave="$root/.deps/dave"

[[ -x $cxx && -f $dave/libdave.a && -d $pacbrew/include ]] || {
    echo "bg-voice: SDK, libdave or PacBrew is missing (run make deps && make dave)" >&2
    exit 1
}

build="$root/build/bg-voice"
rm -rf "$build"
mkdir -p "$build" "$(dirname "$out")"

stub_cc=${PS5_CLANG:-clang}
"$stub_cc" --target=x86_64-pc-freebsd -fPIC -shared -nostdlib -fuse-ld=lld \
    -Wl,-soname,libSceAudioIn.sprx -o "$build/libSceAudioIn.so" \
    "$root/tools/stubs/libSceAudioIn.c"

flags=(-std=c++20 -O2 -Wall -Wextra -fexceptions -frtti
    -I"$root/src" -I"$dave/include" -I"$pacbrew/include" -I"$pacbrew/include/opus")

sources=(
    "$root/payload/bg-voice.cpp"
    "$root/src/voice/engine.cpp"
    "$root/src/voice/mic.cpp"
    "$root/src/audio/mixer.cpp"
    "$root/src/platform/ps5/audio_out.cpp"
    "$root/src/platform/ps5/system.cpp"
    "$root/src/dc/net/dns.cpp"
    "$root/src/dc/net/http.cpp"
    "$root/src/dc/net/tls.cpp"
    "$root/src/dc/net/websocket.cpp"
    "$root/src/dc/util/base64.cpp"
    "$root/src/i18n/i18n.cpp"
)

objects=()
for source in "${sources[@]}"; do
    object="$build/$(basename "$source").o"
    "$cxx" "${flags[@]}" -c "$source" -o "$object"
    objects+=("$object")
done

"$cxx" "${flags[@]}" -o "$out" "${objects[@]}" \
    -Wl,--start-group \
    "$dave/libdave.a" \
    "$pacbrew/lib/libssl.a" "$pacbrew/lib/libcrypto.a" \
    "$pacbrew/lib/libopus.a" "$pacbrew/lib/libsodium.a" "$pacbrew/lib/libz.a" \
    "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a" \
    -Wl,--end-group \
    "$build/libSceAudioIn.so" \
    -lSceAudioOut -lSceUserService -lSceSystemService -lSceNet -lSceSsl \
    -pthread

echo "bg-voice: $out"
