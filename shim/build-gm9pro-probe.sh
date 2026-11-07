#!/bin/bash
# Usage: bash shim/build-gm9pro-probe.sh STOCK_BOOT.img
set -euo pipefail
stock=$(realpath "$1")
cd "$(dirname "$0")"
cc=${CROSS_COMPILE:-aarch64-linux-gnu-}
mkdir -p out/gm9pro
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror)
for source in start.S gm9pro-probe.c; do
    "${cc}gcc" "${flags[@]}" -c "$source" -o "out/gm9pro/${source%.*}.o"
done
"${cc}gcc" "${flags[@]}" -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
    out/gm9pro/start.o out/gm9pro/gm9pro-probe.o -o out/gm9pro/probe.elf
"${cc}objcopy" -O binary out/gm9pro/probe.elf out/gm9pro/Image
python3 ../tools/mkboot-gm9pro.py "$stock" out/gm9pro/Image out/gm9pro/gm9pro-probe.img
