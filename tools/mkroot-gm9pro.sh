#!/bin/bash
# Build a small case-sensitive HFS+ root for the RAM-backed boot test.
set -euo pipefail
init=$(realpath "$1")
output=$(realpath "$(dirname "$2")")/$(basename "$2")
test ! -e "$output"
work=$(mktemp -d)
mounted=0
cleanup() {
    if (( mounted )); then hdiutil detach "$work/mnt" >/dev/null || true; fi
    rm -rf "$work"
}
trap cleanup EXIT
mkdir "$work/mnt"
hdiutil create -size 32m -fs HFSX -layout NONE \
    -format UDRW "$work/root.dmg"
hdiutil attach -quiet -nobrowse -owners on -mountpoint "$work/mnt" "$work/root.dmg"
mounted=1
sudo mkdir -p "$work/mnt/sbin" "$work/mnt/dev" "$work/mnt/private/var"
sudo cp "$init" "$work/mnt/sbin/launchd"
sudo chmod 0755 "$work/mnt/sbin/launchd"
printf 'GM9 HFS root OK\n' | sudo tee "$work/mnt/GM9-HFS-TEST" >/dev/null
sudo chown -R 0:0 "$work/mnt/sbin" "$work/mnt/dev" "$work/mnt/private" "$work/mnt/GM9-HFS-TEST"
hdiutil detach -quiet "$work/mnt"
mounted=0
# UDTO is a flat sector image; no UDIF trailer is passed to XNU's RAM disk.
hdiutil convert -quiet "$work/root.dmg" -format UDTO -o "$work/root"
mv "$work/root.cdr" "$output"
python3 - "$output" <<'PY'
from pathlib import Path
import struct, sys
p = Path(sys.argv[1])
b = p.read_bytes()
assert b[1024:1026] == b'HX', 'Expected case-sensitive HFS+ volume'
block_size, blocks = struct.unpack_from('>II', b, 1064)
assert block_size >= 512 and block_size & (block_size - 1) == 0
assert block_size * blocks <= len(b)
assert len(b) <= 34 * 1024 * 1024, 'RAM root exceeds the boot payload budget'
print(f'HFS root: {len(b)} bytes, {blocks} allocation blocks of {block_size}')
PY
