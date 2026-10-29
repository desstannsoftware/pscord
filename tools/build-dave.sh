#!/usr/bin/env bash
# ps5-discord - Builds Discord's DAVE end-to-end encryption (libdave) and the MLS
# library under it (Cisco mlspp) into one static archive for the console.
# Copyright (C) 2026 Desstann
# SPDX-License-Identifier: MIT
#
# Output: .deps/dave/libdave.a and .deps/dave/include. Sources are fetched into
# .deps/src at the revisions libdave's own vcpkg ports pin.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src="$root/.deps/src"
out="$root/.deps/dave"
sdk_root="$root/.deps/native/ps5-payload-sdk"
pacbrew="$root/.deps/pacbrew/v0.40.2/sysroot/user/homebrew"
libdave_rev=6396c90
mlspp_rev=1cc50a124a3bc4e143a787ec934280dc70c1034d
json_url=https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp

if [[ ! -f $pacbrew/include/openssl/evp.h ]]; then
    echo "PacBrew OpenSSL headers missing at $pacbrew/include/openssl (run: make pacbrew)" >&2
    exit 2
fi

mkdir -p "$src"
[[ -d $src/libdave ]] || git clone -q https://github.com/discord/libdave.git "$src/libdave"
git -C "$src/libdave" checkout -q "$libdave_rev" 2>/dev/null || true
if [[ ! -d $src/mlspp ]]; then
    git clone -q https://github.com/cisco/mlspp.git "$src/mlspp"
fi
git -C "$src/mlspp" checkout -q "$mlspp_rev"
[[ -f $src/json.hpp ]] || wget -q "$json_url" -O "$src/json.hpp"

# The console's libc has no locales, so nothing may reach libc++'s iostreams:
# libdave logs through tools/dave-overlay, and mlspp loses its stream helpers.
git -C "$src/mlspp" checkout -q -- .
python3 - "$src/mlspp" <<'PATCH'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
p = root / "lib/bytes/src/bytes.cpp"
s = p.read_text()
s = s.replace("#include <iomanip>\n#include <iostream>\n", "").replace("#include <sstream>\n", "")
s = re.sub(r"std::string\nto_hex\(const bytes& data\)\n\{.*?\n\}\n", '''std::string
to_hex(const bytes& data)
{
  static const char digits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(data.size() * 2);
  for (const auto& byte : data) {
    hex += digits[byte >> 4];
    hex += digits[byte & 0x0f];
  }
  return hex;
}
''', s, count=1, flags=re.S)
s = re.sub(r"std::ostream&\noperator<<\(std::ostream& out, const bytes& data\)\n\{.*?\n\}\n", "", s, count=1, flags=re.S)
p.write_text(s)
p = root / "src/treekem.cpp"
p.write_text(p.read_text().replace("#include <iostream>\n", "").replace("        dump();\n", ""))
p = root / "include/mls/treekem.h"
p.write_text(p.read_text().replace("#define ENABLE_TREE_DUMP 1", "#define ENABLE_TREE_DUMP 0"))
PATCH

rm -rf "$out"
mkdir -p "$out/obj" "$out/include/mls" "$out/include/nlohmann"
printf '#pragma once\n#define MLS_NAMESPACE mlspp\n' > "$out/include/namespace.h"
cp "$src/json.hpp" "$out/include/nlohmann/json.hpp"

mls="$src/mlspp"
dave="$src/libdave/cpp"
includes=(
    -I"$root/tools/dave-overlay" -I"$out/include" -I"$mls/include" -I"$mls/lib/bytes/include" -I"$mls/lib/hpke/include"
    -I"$mls/lib/tls_syntax/include" -I"$mls/third_party" -I"$dave/includes" -I"$dave/src"
    -I"$pacbrew/include"
)
flags=(-std=c++17 -O2 -fexceptions -frtti -ffunction-sections -fdata-sections -w
    -DDISABLE_GREASE -DWITH_OPENSSL3 "${includes[@]}")

sources=()
while IFS= read -r -d '' f; do sources+=("$f"); done < <(
    find "$mls/src" "$mls/lib/bytes/src" "$mls/lib/hpke/src" "$mls/lib/tls_syntax/src" -name '*.cpp' -print0
    find "$dave/src" -name '*.cpp' ! -name '*_wasm.cpp' ! -name '*_win.cpp' ! -name '*_apple.cpp' \
        ! -name 'boringssl_cryptor.cpp' ! -name 'bindings_capi.cpp' ! -path '*persisted_key*' \
        ! -name 'logger.cpp' -print0
    printf '%s\0' "$dave/src/mls/persisted_key_pair_null.cpp" "$root/tools/dave-overlay/logger.cpp"
)

compile() {
    local source=$1
    local name=${source#"$root/"}
    local object="$out/obj/$(echo "${name#.}" | tr '/' '_').o"
    PS5_PAYLOAD_SDK="$sdk_root" PS5_CLANG="${PS5_CLANG:-clang-18}" \
        sh "$root/tooling/prospero-clang18" "${flags[@]}" -c "$source" -o "$object"
}
export -f compile
export root src out sdk_root
export flags_string="${flags[*]}"
failed=0
for source in "${sources[@]}"; do
    compile "$source" &
    while (( $(jobs -rp | wc -l) >= $(nproc) )); do wait -n || failed=1; done
done
while (( $(jobs -rp | wc -l) > 0 )); do wait -n || failed=1; done
(( failed == 0 )) || { echo "libdave build failed" >&2; exit 1; }

"$sdk_root/bin/llvm-ar" rcs "$out/libdave.a" "$out"/obj/*.o
cp -r "$dave/includes/dave" "$out/include/"
cp "$root/tools/dave-overlay/dave/logger.h" "$out/include/dave/logger.h"
echo "wrote $out/libdave.a ($(ls "$out"/obj/*.o | wc -l) objects)"
