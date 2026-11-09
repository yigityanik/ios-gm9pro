#!/bin/bash
set -euo pipefail
stock=$(realpath "$1")
cd "$(dirname "$0")/.."
mkdir -p shim/out/gm9pro-hfs
python3 - <<'PY'
from pathlib import Path
s=Path('shim/gm9pro-probe.c').read_text()
s=s.replace('static void hex(const char *name, uint64_t value)\n{',
    'static int hfs_quiet;\nstatic void hex(const char *name, uint64_t value)\n{\n    if (hfs_quiet) return;')
assert 'if (hfs_quiet) return;' in s
marker='static int64_t cpu_on'
assert s.count(marker)==1
s=s.replace(marker, '#include "gm9pro-hfs-probe.inc"\n'+marker)
assert s.count('        emmc_probe();')==1
s=s.replace('        emmc_probe();','        hfs_probe();')
Path('shim/out/gm9pro-hfs/probe.c').write_text(s)
PY
cd shim
cc=${CROSS_COMPILE:-aarch64-linux-gnu-}
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror -Wno-unused-function -DGM9PRO_ENTRY_TRACE)
"${cc}gcc" "${flags[@]}" -c start.S -o out/gm9pro-hfs/start.o
"${cc}gcc" "${flags[@]}" -I. -c out/gm9pro-hfs/probe.c -o out/gm9pro-hfs/probe.o
"${cc}gcc" "${flags[@]}" -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N out/gm9pro-hfs/start.o out/gm9pro-hfs/probe.o -o out/gm9pro-hfs/probe.elf
"${cc}objcopy" -O binary out/gm9pro-hfs/probe.elf out/gm9pro-hfs/Image
python3 ../tools/mkboot-gm9pro.py "$stock" out/gm9pro-hfs/Image out/gm9pro-hfs/gm9pro-hfs-probe.img
