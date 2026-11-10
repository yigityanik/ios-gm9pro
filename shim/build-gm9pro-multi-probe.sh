#!/bin/bash
set -euo pipefail
stock=$(realpath "$1")
cd "$(dirname "$0")/.."
mkdir -p shim/out/gm9pro-multi
python3 - <<'PY'
from pathlib import Path
s = Path('shim/gm9pro-probe.c').read_text()
old = 'static void hex(const char *name, uint64_t value)\n{'
assert s.count(old) == 1
s = s.replace(old, 'static int multi_quiet;\n' + old + '\n    if (multi_quiet) return;')
marker = 'static int64_t cpu_on'
assert s.count(marker) == 1
s = s.replace(marker, '#include "gm9pro-emmc-multi-probe.inc"\n' + marker)
assert s.count('        emmc_probe();') == 1
s = s.replace('        emmc_probe();', '        multi_probe();')
Path('shim/out/gm9pro-multi/probe.c').write_text(s)
ld = Path('shim/shim.ld').read_text()
assert ld.count('\t_end = .;') == 1
ld = ld.replace('\t_end = .;', '\t. = ALIGN(4096);\n\t.probe_buffers (NOLOAD) : { *(.probe_buffers) }\n\t_end = .;')
Path('shim/out/gm9pro-multi/probe.ld').write_text(ld)
PY
cd shim
cc=${CROSS_COMPILE:-aarch64-linux-gnu-}
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror -Wno-unused-function -DGM9PRO_ENTRY_TRACE)
"${cc}gcc" "${flags[@]}" -c start.S -o out/gm9pro-multi/start.o
"${cc}gcc" "${flags[@]}" -I. -c out/gm9pro-multi/probe.c -o out/gm9pro-multi/probe.o
"${cc}gcc" "${flags[@]}" -static -T out/gm9pro-multi/probe.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N out/gm9pro-multi/start.o out/gm9pro-multi/probe.o -o out/gm9pro-multi/probe.elf
"${cc}objcopy" -O binary out/gm9pro-multi/probe.elf out/gm9pro-multi/Image
python3 ../tools/mkboot-gm9pro.py "$stock" out/gm9pro-multi/Image out/gm9pro-multi/gm9pro-emmc-multi-probe.img
