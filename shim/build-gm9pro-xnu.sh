#!/bin/bash
# Usage: bash shim/build-gm9pro-xnu.sh STOCK_BOOT.img KERNEL_MACHO
set -euo pipefail
stock=$(realpath "$1")
kernel=$(realpath "$2")
cd "$(dirname "$0")"
mkdir -p out/gm9pro-xnu
python3 ../tools/mkadt-gm9pro.py out/gm9pro-xnu/gm9pro.adt
init="$(dirname "$kernel")/gm9pro-init.bin"
root="$(dirname "$kernel")/gm9pro-root.img"
if [[ -f "$root" ]]; then init="$root"; fi
args=()
cmdline='debug=0x14e serial=3 cpus=1 keepsyms=1 -noprogress gm9emmc=2 rd=md0'
if [[ -f "$init" ]]; then
    args+=(--ramdisk "$init")
    cmdline='debug=0x14e serial=3 cpus=1 keepsyms=1 -noprogress gm9emmc=0 rd=md0'
fi
case "${GM9PRO_BOOT_MODE:-ram}" in
    ram) ;;
    emmc)
        part=${GM9PRO_ROOT_PARTITION:-userdata}
        [[ "$part" =~ ^[A-Za-z0-9_]+$ ]] || { echo 'Invalid root partition name' >&2; exit 1; }
        args=()
        cmdline="debug=0x14e serial=3 cpus=1 keepsyms=1 -noprogress rd=emmcroot0 emmcroot=$part unentitled_ios_sim_launch=1 ignition_level=1"
        if [[ "${GM9PRO_ENABLE_WRITES:-0}" == 1 ]]; then
            [[ "$part" == userdata ]] || { echo 'Writes are limited to userdata' >&2; exit 1; }
            [[ "${GM9PRO_COW:-0}" != 1 ]] || { echo 'COW and physical writes cannot both be requested' >&2; exit 1; }
            cmdline+=" gm9rw=1"
        fi
        if [[ "${GM9PRO_COW:-0}" == 1 ]]; then
            [[ "$part" == userdata ]] || { echo 'COW is limited to userdata' >&2; exit 1; }
            cmdline+=" gm9cow=1"
        fi
        if [[ "${GM9PRO_UNSECURE_LAUNCHD_CACHE:-0}" == 1 ]]; then
            cmdline+=" launchd_unsecure_cache=1"
        fi
        ;;
    *) echo 'GM9PRO_BOOT_MODE must be ram or emmc' >&2; exit 1 ;;
esac
if [[ "${GM9PRO_SHOW_PROGRESS:-0}" == 1 ]]; then
    cmdline=${cmdline/ -noprogress/}
fi
if [[ "${GM9PRO_QUIET_FB:-0}" == 1 ]]; then
    cmdline+=" sweetfb_noconsole=1"
fi
python3 ../tools/mkpayload.py "$kernel" out/gm9pro-xnu/gm9pro.adt out/gm9pro-xnu/payload \
    --cmdline "$cmdline" "${args[@]}"
cc=${CROSS_COMPILE:-aarch64-linux-gnu-}
bytes=$(wc -c < out/gm9pro-xnu/payload)
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror -Wno-unused-function -DGM9PRO_ENTRY_TRACE -DGM9PRO_XNU_LOADER "-DPAYLOAD_BYTES=$bytes")
objects=()
for src in start.S gm9pro-probe.c gm9pro-console.c gm9pro-gic-prepare.c loader.c fdt.c; do
    obj="out/gm9pro-xnu/${src%.*}.o"
    "${cc}gcc" "${flags[@]}" -c "$src" -o "$obj"
    objects+=("$obj")
done
"${cc}gcc" "${flags[@]}" -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
    "${objects[@]}" -o out/gm9pro-xnu/shim.elf
"${cc}objcopy" -O binary out/gm9pro-xnu/shim.elf out/gm9pro-xnu/Image
end=$("${cc}nm" out/gm9pro-xnu/shim.elf | awk '$3=="_end" {print $1}')
truncate -s "$((16#$end))" out/gm9pro-xnu/Image
cat out/gm9pro-xnu/payload >> out/gm9pro-xnu/Image
# ABL's observed DTB destination is 0x83200000; the stock kernel address is
# 0x80008000. Refuse an appended payload that would run into that DTB.
image_bytes=$(wc -c < out/gm9pro-xnu/Image)
if (( image_bytes > 0x83200000 - 0x80008000 )); then
    echo 'XNU payload overlaps the observed ABL DTB destination' >&2
    exit 1
fi
python3 ../tools/mkboot-gm9pro.py "$stock" out/gm9pro-xnu/Image out/gm9pro-xnu/gm9pro-xnu-entry.img
