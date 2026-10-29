#!/usr/bin/env python3
# ps5-discord - Packs an app folder into an OTA bundle and writes its update.json.
# Layout (little-endian, read by src/update/updater.cpp): "PSCUPD1\n", u32 file
# count, then per file u16 path length, UTF-8 path, u64 size, bytes.
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import json
import struct
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("app", type=Path, help="built app folder (dist/PPSA99190)")
parser.add_argument("--version", required=True)
parser.add_argument("--url", required=True, help="where the bundle will be downloaded from")
parser.add_argument("--notes", default="")
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()

files = sorted(p for p in args.app.rglob("*") if p.is_file() and p.name != ".DS_Store")
# eboot.bin goes last so everything it loads is already written.
files.sort(key=lambda p: p.name == "eboot.bin")
args.out.mkdir(parents=True, exist_ok=True)
bundle = args.out / "pscord-update.bin"
digest = hashlib.sha256()
with bundle.open("wb") as out:
    def put(data: bytes) -> None:
        out.write(data)
        digest.update(data)

    put(b"PSCUPD1\n" + struct.pack("<I", len(files)))
    for path in files:
        rel = path.relative_to(args.app).as_posix().encode()
        data = path.read_bytes()
        put(struct.pack("<H", len(rel)) + rel + struct.pack("<Q", len(data)))
        put(data)

manifest = {
    "version": args.version,
    "bundle": args.url,
    "size": bundle.stat().st_size,
    "sha256": digest.hexdigest(),
    "notes": args.notes,
}
(args.out / "update.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
print(f"{bundle} {manifest['size']} bytes, {len(files)} files, sha256 {manifest['sha256']}")
