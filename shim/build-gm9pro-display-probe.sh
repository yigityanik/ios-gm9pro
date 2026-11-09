#!/bin/bash
set -euo pipefail
stock=$(realpath "$1")
cd "$(dirname "$0")/.."
mkdir -p shim/out/gm9pro-display
python3 - <<'PY'
from pathlib import Path
import os
import re

s = Path('shim/gm9pro-probe.c').read_text()
start = s.index('static void emmc_probe(void)')
s = s[:start] + r'''static void display_probe(void)
{
    const uintptr_t mdp = 0x0c900000UL;
    const uintptr_t intr = mdp + 0x1014;
    const uintptr_t ctl = mdp + 0x2000;
    const uintptr_t vig = mdp + 0x5000;
    const uintptr_t fb_pa = 0x9d400000UL;
    const uint32_t width = 1080, height = 2160, stride = 4320;
    const uint32_t done_bit = 1U << 8; /* PP0_DONE, same MDP5 command-mode path used by Pixel 1 */

    puts_uart("GM9 DISPLAY TEST: preserve ABL MDP setup; inspect before touching pixels\n");
    hex("MDP interrupt status", read32(intr));
    hex("MDP CTL0 config", read32(ctl));
    hex("MDP CTL0 flush", read32(ctl + 0x18));
    hex("MDP CTL0 start", read32(ctl + 0x1c));
    hex("MDP PP0 base", mdp + 0x71000);
    hex("MDP PP0 status", read32(mdp + 0x71000));
    hex("DSI0 control", read32(0x0c994000UL));
    hex("DSI0 status", read32(0x0c994000UL + 0x110));
    hex("VIG0 size", read32(vig));
    hex("VIG0 source", read32(vig + 0x14));
    hex("VIG0 stride", read32(vig + 0x24));
    hex("VIG0 format", read32(vig + 0x30));
    hex("VIG0 unpack", read32(vig + 0x34));

    if (read32(vig + 0x14) != fb_pa ||
        read32(vig) != 0x08700438U ||
        read32(vig + 0x24) != stride ||
        read32(vig + 0x30) != 0x000237ffU ||
        read32(vig + 0x34) != 0x03020001U) {
        puts_uart("Framebuffer handoff differs from measured ABL state; no writes made\n");
        return;
    }

    /* The 8.9 MiB image stays inside the 36 MiB splash reservation. */
    volatile uint32_t *fb = (volatile uint32_t *)fb_pa;
    for (uint32_t y = 0; y < height; y++) {
        uint32_t c = y < 540 ? 0xff202020U : y < 1080 ? 0xff606060U :
                     y < 1620 ? 0xffa0a0a0U : 0xffe0e0e0U;
        volatile uint32_t *row = (volatile uint32_t *)((uintptr_t)fb + (uintptr_t)y * stride);
        for (uint32_t x = 0; x < width; x++) row[x] = c;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    puts_uart("Framebuffer pattern written; waiting 50 ms before one CTL0 start\n");
    uint64_t settle = ticks() + freq() / 20;
    while (ticks() < settle) __asm__ volatile("yield");

    write32(intr + 4, done_bit); /* clear only PP0_DONE */
    __asm__ volatile("dsb sy" ::: "memory");
    write32(ctl + 0x1c, 1);      /* CTL0_START, leave ABL's pipe/mixer/DSI setup intact */
    __asm__ volatile("dsb sy" ::: "memory");

    uint64_t deadline = ticks() + freq() / 10;
    while (!(read32(intr) & done_bit) && ticks() < deadline)
        __asm__ volatile("yield");
    hex("MDP interrupt after start", read32(intr));
    hex("MDP CTL0 start after", read32(ctl + 0x1c));
    puts_uart((read32(intr) & done_bit) ?
        "GM9 DISPLAY TEST: PP0_DONE received\n" :
        "GM9 DISPLAY TEST: no PP0_DONE within 100 ms\n");
    puts_uart("No eMMC command or persistent partition write was issued\n");
}
''' + s[start:]
s = s.replace('puts_uart("eMMC controller read-only inspection\\n");',
              'puts_uart("MDP handoff read-only inspection\\n");')
s = s.replace('    hex("SDHCI present state", read32(EMMC + 0x24));\n'
              '    hex("SDHCI capabilities", read32(EMMC + 0x40));\n'
              '    hex("SDHCI clock/timeout/reset", read32(EMMC + 0x2c));\n'
              '    hex("SDHCI host version", *(volatile uint16_t *)(EMMC + 0xfe));',
              '    hex("MDP base", 0x0c900000UL);')
s = s.replace('        emmc_probe();', '        display_probe();')
s = s.replace('PROBE 5 COMPLETE; waiting for hardware reset',
              'DISPLAY TEST COMPLETE; waiting for hardware reset')
if os.environ.get('GM9PRO_DISPLAY_REPEAT') == '1':
    begin = s.index('static void display_probe(void)')
    end = s.index('static void emmc_probe(void)', begin)
    s = s[:begin] + Path('shim/gm9pro-display-repeat.inc').read_text() + '\n' + s[end:]
    variant = int(os.environ.get('GM9PRO_DISPLAY_VARIANT', '0'))
    if variant not in (0, 1, 2):
        raise ValueError('Display variant must be 0, 1 or 2')
    s = '#define GM9_DISPLAY_VARIANT %d\n' % variant + s
Path('shim/out/gm9pro-display/probe.c').write_text(s)
entry = Path('shim/start.S').read_text()
# The separate 9 MiB buffer puts stacks beyond ADR's +/-1 MiB reach.
entry, count = re.subn(r'adr\s+(x\d+),\s*(_stack_top|_bss_end|_sec_stacks)',
    lambda m: 'adrp\t%s, %s\n\tadd\t%s, %s, :lo12:%s' %
        (m[1], m[2], m[1], m[1], m[2]), entry)
assert count == 3, 'Display probe entry layout changed'
Path('shim/out/gm9pro-display/start.S').write_text(entry)
PY
cd shim
cc=${CROSS_COMPILE:-aarch64-linux-gnu-}
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror -Wno-unused-function -DGM9PRO_ENTRY_TRACE)
"${cc}gcc" "${flags[@]}" -c out/gm9pro-display/start.S -o out/gm9pro-display/start.o
"${cc}gcc" "${flags[@]}" -c out/gm9pro-display/probe.c -o out/gm9pro-display/probe.o
"${cc}gcc" "${flags[@]}" -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N out/gm9pro-display/start.o out/gm9pro-display/probe.o -o out/gm9pro-display/probe.elf
"${cc}objcopy" -O binary out/gm9pro-display/probe.elf out/gm9pro-display/Image
python3 ../tools/mkboot-gm9pro.py "$stock" out/gm9pro-display/Image out/gm9pro-display/gm9pro-display-test.img
