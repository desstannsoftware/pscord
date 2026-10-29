#!/usr/bin/env bash
# ps5-homebrew-ui - Rebuild the baked SDF fonts in assets/fonts.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Each line of the table is: source TTF, output name, pixel size, SDF range.
# The baker takes two more arguments, the atlas size and the glyph set: an
# app that shows names it did not write can bake `2048 european` (accented
# Latin including Vietnamese, Greek, Cyrillic) instead of the `1024` basic
# set used here. Keep european atlases at 2048 — larger full-BMP bakes
# bloated startup and stuck consoles on the splash.
# To add a typeface, drop its TTF and licence into third_party/fonts, add a
# line here, run `make fonts`, and load the new .huifont where the others are
# loaded (src/main.cpp and host/snapshot_main.cpp).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
mkdir -p "$root/build/host" "$root/assets/fonts"
"$cxx" -std=c++20 -O2 -w "$root/tools/font-baker/bake_font.cpp" -o "$root/build/host/bake_font"
while read -r source output size range atlas glyphs; do
    [[ -n $source ]] || continue
    echo "→ $output"
    "$root/build/host/bake_font" "$root/third_party/fonts/$source" \
        "$root/assets/fonts/$output.huifont" "$size" "$range" ${atlas:-1024} ${glyphs:-}
done <<'FONTS'
Inter-Regular.ttf inter-regular 56 8 2048 european
Inter-SemiBold.ttf inter-semibold 56 8 2048 european
Montserrat-Medium.ttf montserrat-medium 56 8 2048 european
DejaVuSansMono.ttf dejavu-sans-mono 52 8 2048 european
PressStart2P-Regular.ttf press-start-2p 32 4
PatrickHand-Regular.ttf patrick-hand 56 8
FONTS
cp "$root"/third_party/fonts/*-LICENSE.txt "$root/assets/fonts/"
