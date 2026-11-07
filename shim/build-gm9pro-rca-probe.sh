#!/bin/bash
# Build a read-only CMD13 / inherited RCA diagnostic.
set -euo pipefail
stock=$(realpath "$1")
cd "$(dirname "$0")/.."
out=shim/out/gm9pro-rca-probe
mkdir -p "$out"
python3 - "$out/probe.c" <<'PY'
from pathlib import Path
import sys
s = Path('shim/gm9pro-probe.c').read_text()
s = s.replace('GM9PRO SHIM PROBE 5', 'GM9PRO EMMC RCA PROBE')
s = s.replace('PROBE 5 COMPLETE', 'RCA PROBE COMPLETE')
anchor = 'static int64_t cpu_on(uint64_t target, uint64_t idx)'
assert s.count(anchor) == 1
s = s.replace(anchor, '#include "gm9pro-rca-probe.inc"\n' + anchor)
old = '        emmc_probe();\n#endif'
assert s.count(old) == 1
s = s.replace(old, '        emmc_probe();\n        gm9_rca_probe();\n#endif')
Path(sys.argv[1]).write_text(s)
PY
cc=${CROSS_COMPILE:-aarch64-linux-gnu-}
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align
    -fno-stack-protector -Wall -Wextra -Werror -DGM9PRO_ENTRY_TRACE -Ishim)
"${cc}gcc" "${flags[@]}" -c shim/start.S -o "$out/start.o"
"${cc}gcc" "${flags[@]}" -c "$out/probe.c" -o "$out/probe.o"
"${cc}gcc" "${flags[@]}" -static -T shim/shim.ld -Wl,--build-id=none \
    -Wl,--no-warn-rwx-segments -Wl,-N "$out/start.o" "$out/probe.o" -o "$out/probe.elf"
"${cc}objcopy" -O binary "$out/probe.elf" "$out/Image"
python3 tools/mkboot-gm9pro.py "$stock" "$out/Image" "$out/probe.img"
