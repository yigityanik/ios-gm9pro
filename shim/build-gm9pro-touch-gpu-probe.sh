#!/bin/bash
set -euo pipefail
stock=$(realpath "$1")
cd "$(dirname "$0")/.."
mkdir -p shim/out/gm9pro-touch-gpu
python3 - <<'PY'
from pathlib import Path
s = Path('shim/gm9pro-probe.c').read_text()
anchor = 'void secondary_main(uint64_t idx)'
assert s.count(anchor) == 1
s = s.replace(anchor, '#include "gm9pro-touch-gpu-probe.inc"\n' + anchor)
assert s.count('        emmc_probe();') == 1
s = s.replace('        emmc_probe();', '        touch_gpu_probe();')
begin = s.index('    puts_uart("eMMC controller read-only inspection')
end = s.index('    if (!cpu[4].ready', begin)
s = s[:begin] + s[end:]
Path('shim/out/gm9pro-touch-gpu/probe.c').write_text(s)
PY
cd shim
cc=${CROSS_COMPILE:-aarch64-linux-gnu-}
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror -Wno-unused-function -DGM9PRO_ENTRY_TRACE)
"${cc}gcc" "${flags[@]}" -c start.S -o out/gm9pro-touch-gpu/start.o
"${cc}gcc" "${flags[@]}" -I. -c out/gm9pro-touch-gpu/probe.c -o out/gm9pro-touch-gpu/probe.o
"${cc}gcc" "${flags[@]}" -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N out/gm9pro-touch-gpu/start.o out/gm9pro-touch-gpu/probe.o -o out/gm9pro-touch-gpu/probe.elf
"${cc}objcopy" -O binary out/gm9pro-touch-gpu/probe.elf out/gm9pro-touch-gpu/Image
python3 ../tools/mkboot-gm9pro.py "$stock" out/gm9pro-touch-gpu/Image out/gm9pro-touch-gpu/gm9pro-touch-gpu-probe.img
