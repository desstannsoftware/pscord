#!/usr/bin/env bash
# PSCord - helper script retained from the build kit (unused by CI).
# Copyright (C) 2026 Desstann
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host=${PS5_HOST:-192.168.1.100}
port=${FTP_PORT:-2121}
homebrew=${PS5_HOMEBREW:-/data/homebrew}
work=${PC_CONVERT_WORK:-$root/build/pc-convert}
threads=${FTP_THREADS:-4}

[[ $# -ge 1 && -f $1 ]] || { echo "usage: tools/pc-convert.sh <file.pkg> [--no-upload] [--keep]" >&2; exit 2; }
pkg=$1
shift
upload=1
keep=0
for arg in "$@"; do
    case $arg in
        --no-upload) upload=0 ;;
        --keep) keep=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

mkdir -p "$work"
bin="$work/folderize-extract"
srcs=("$root"/src/pkg/{cnt,crypto,extract,ffpfsc,fih,fsutil,helper_client,kraken,kraken_decoder,launch_fix,listing,log,naps,pfs,pfsc,publish,sha256,sha3,telemetry}.cpp
      "$root/tools/folderize_extract_main.cpp")
needs_build=0
[[ -x $bin ]] || needs_build=1
for s in "${srcs[@]}" "$root"/src/pkg/*.hpp; do
    [[ $s -nt $bin ]] && needs_build=1
done
if (( needs_build )); then
    echo "==> building $bin"
    cc -O2 -c "$root/third_party/aes/aes.c" -o "$work/aes.o" -DECB=1 -DAES128=1
    c++ -std=c++20 -O2 -w -I"$root/src" -I"$root/third_party" "${srcs[@]}" "$work/aes.o" -lz -o "$bin"
fi

out="$work/$(basename "$pkg" .pkg)"
rm -rf "$out"
echo "==> extracting $pkg"
start=$(date +%s)
log=$(PKG_KRAKEN_INPROCESS=1 "$bin" "$pkg" "$out" --launch-fix "$root/assets/ampr/libSceAmpr.sprx" 2>/dev/null)
echo "$log"
tid=$(sed -n 's/^title_id=//p' <<<"$log")
[[ $tid =~ ^[A-Z]{4}[0-9]{5}$ ]] || { echo "could not read title id" >&2; exit 1; }
echo "==> extracted in $(( $(date +%s) - start ))s ($(du -sh "$out" | cut -f1))"

(( upload )) || { echo "dump: $out"; exit 0; }

echo "==> uploading to ftp://$host:$port$homebrew/$tid-app0 ($threads connections)"
python3 - "$out" "$host" "$port" "$homebrew" "$tid" "$threads" <<'PY'
import os, sys, time, threading, queue
from ftplib import FTP, error_perm, error_reply

local, host, port, homebrew, tid, threads = sys.argv[1:]
port, threads = int(port), int(threads)
final = f"{homebrew}/{tid}-app0"
part = final + ".part"

def connect():
    ftp = FTP()
    ftp.connect(host, port, timeout=120)
    ftp.login("anonymous", "")
    ftp.set_pasv(True)
    return ftp

# The PS5 FTP server ignores the MLSD path argument and lists the cwd, so
# always cwd into the directory first; a failed cwd means it does not exist.
def rmtree(ftp, path):
    if not path.startswith(homebrew + "/") or path.count("/") <= homebrew.count("/"):
        sys.exit(f"refusing to delete {path}")
    try:
        ftp.cwd(path)
    except error_perm:
        return
    if ftp.pwd().rstrip("/") != path:
        sys.exit(f"cwd mismatch while deleting {path}")
    entries = list(ftp.mlsd())
    ftp.cwd("/")
    for name, facts in entries:
        if name in (".", ".."):
            continue
        p = f"{path}/{name}"
        if facts.get("type") == "dir":
            rmtree(ftp, p)
        else:
            sendcmd(ftp, "DELE " + p)
    sendcmd(ftp, "RMD " + path)

# DELE answers "226 File deleted", which ftplib treats as an unexpected reply.
def sendcmd(ftp, cmd):
    try:
        ftp.sendcmd(cmd)
    except error_reply:
        pass

files, dirs = [], []
for base, ds, fs in os.walk(local):
    rel = os.path.relpath(base, local)
    dirs.append("" if rel == "." else rel)
    for f in fs:
        r = os.path.normpath(os.path.join(rel, f))
        files.append((r, os.path.getsize(os.path.join(local, r))))
files.sort(key=lambda t: -t[1])
total = sum(s for _, s in files)

ctl = connect()
rmtree(ctl, part)
for d in sorted(dirs, key=len):
    try:
        ctl.mkd(part + ("/" + d if d else ""))
    except error_perm:
        pass

work = queue.Queue()
for item in files:
    work.put(item)
sent = 0
lock = threading.Lock()
errors = []
start = time.time()

def worker():
    global sent
    ftp = connect()
    while True:
        try:
            rel, size = work.get_nowait()
        except queue.Empty:
            break
        def cb(chunk):
            global sent
            with lock:
                sent += len(chunk)
        try:
            with open(os.path.join(local, rel), "rb") as fh:
                ftp.storbinary(f"STOR {part}/{rel}", fh, blocksize=1 << 20, callback=cb)
        except Exception as e:
            errors.append(f"{rel}: {e}")
            break
    ftp.quit()

pool = [threading.Thread(target=worker) for _ in range(min(threads, len(files)) or 1)]
for t in pool:
    t.start()
while any(t.is_alive() for t in pool):
    time.sleep(2)
    el = time.time() - start
    print(f"\r    {sent/1e9:.2f}/{total/1e9:.2f} GB  {sent/el/1e6:.1f} MB/s", end="", flush=True)
print()
if errors:
    sys.exit("upload failed: " + "; ".join(errors[:3]))

ctl = connect()
rmtree(ctl, final)
ctl.rename(part, final)
ctl.quit()
print(f"==> published {final} in {time.time()-start:.0f}s")
PY

(( keep )) || rm -rf "$out"
