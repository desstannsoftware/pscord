#!/usr/bin/env bash
# Signal Companion - Runs a command in the build container with the project mounted at the same path.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Prefer a x86_64 Colima profile (`colima start --arch x86_64`). --platform keeps
# runs correct if the daemon is still multi-arch.
exec docker run --rm --platform linux/amd64 -u "$(id -u):$(id -g)" -v "$root:$root" -w "$root" \
    -e HOME="$root/.home" -e CCACHE_DIR="$root/.home/ccache" signal-companion-build bash -lc "$*"
