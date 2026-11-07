#!/bin/bash
set -euo pipefail
stock=$(realpath "$1")
cd "$(dirname "$0")"
cc=${CROSS_COMPILE:-aarch64-linux-gnu-}
mkdir -p out/gm9pro-gic
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror -DGM9PRO_ENTRY_TRACE)
for src in start.S gm9pro-gic.c gm9pro-console.c; do
    "${cc}gcc" "${flags[@]}" -c "$src" -o "out/gm9pro-gic/${src%.*}.o"
done
"${cc}gcc" "${flags[@]}" -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
    out/gm9pro-gic/start.o out/gm9pro-gic/gm9pro-gic.o out/gm9pro-gic/gm9pro-console.o -o out/gm9pro-gic/probe.elf
"${cc}objcopy" -O binary out/gm9pro-gic/probe.elf out/gm9pro-gic/Image
python3 ../tools/mkboot-gm9pro.py "$stock" out/gm9pro-gic/Image out/gm9pro-gic/gm9pro-gic.img
