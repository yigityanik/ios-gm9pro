#!/usr/bin/env python3
"""Apply GM9 Pro bring-up overrides after the first 18 SWEET patches."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1])
repo = Path(__file__).resolve().parent.parent

def update(path, transform):
    p = source / path
    old = p.read_text()
    new = transform(old)
    if new == old:
        raise RuntimeError(f"Expected source change missing: {path}")
    p.write_text(new)

update('makedefs/MakeInc.def', lambda s: s.replace(
    '-mcpu=kryo', '-mcpu=cortex-a73'))

# Xcode 16's IIG emits a separate kernel message-content pointer. Keep it
# initialized for callers that only fill the original four IORPC fields.
update('iokit/IOKit/IORPC.h', lambda s: s.replace(
    '\tuint32_t           replySize;\n',
    '\tuint32_t           replySize;\n#ifdef __cplusplus\n'
    '\tIORPCMessage * kernelContent = nullptr;\n#else\n'
    '\tIORPCMessage * kernelContent;\n#endif\n'))

def uuid_hash_check(s):
    marker = 'IOPlatformExpertDevice::generatePlatformUUID( void )\n{'
    if s.count(marker) != 1:
        raise RuntimeError('Platform UUID function changed')
    return s.replace(marker, marker + '''
    /* Check the provider before platform identity first depends on it. */
    {
        SHA1_CTX context;
        unsigned char digest[20];
        static const unsigned char expected[20] = {
            0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
            0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d
        };
        SHA1Init(&context);
        SHA1Update(&context, "abc", 3);
        SHA1Final(digest, &context);
        if (memcmp(digest, expected, sizeof(expected)))
            panic("GM9 SHA1 known-answer test failed");
        IOLog("GM9 SHA1 known-answer test passed\\n");
    }
''')
update('iokit/Kernel/IOPlatformExpert.cpp', uuid_hash_check)

def function(text, name, body):
    match = re.search(r"\b" + name + r"\([^;{}]*\)\s*\{", text)
    if not match:
        raise RuntimeError(f"Function missing: {name}")
    begin = match.end() - 1
    level, end = 1, begin + 1
    while level:
        level += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[:begin] + '{\n' + body + '\n}' + text[end:]

# Keep the original mapping lifecycle, but map the UART's 32 MiB block as
# Device memory. The UART itself sits 0x170000 bytes into that block.
update('osfmk/arm64/arm_vm_init.c', lambda s: s.replace('0x9c000000', '0x0c000000').replace(
    'ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)',
    'ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE)'))

def entry(s):
    start = s.index('\t/* Stage 0:')
    end = s.index('#endif', start)
    s = s[:start] + '''\t/* UART marker before stack/page-table setup. */
\tMOV64 x0, 0x0c170008
\tmov w1, #0xffff
Lgm9_uart_wait:
\tldr w2, [x0]
\ttbnz w2, #3, Lgm9_uart_send
\tsubs w1, w1, #1
\tb.ne Lgm9_uart_wait
\tb Lgm9_uart_done
Lgm9_uart_send:
\tmov w1, #1
\tstr w1, [x0, #0x38]
\tmov w1, #88
\tstr w1, [x0, #0x68]
\tdsb sy
Lgm9_uart_done:
''' + s[end:]
    return s.replace('0x9c000000', '0x0c000000').replace(
        'ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)',
        'ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE)')
update('osfmk/arm64/start.s', entry)

uart = r'''/* GM9 Pro early UART, including the final kernel VA alias. */
#include <arm64/sweet_early.h>
#if defined(SWEET)
#include <kern/misc_protos.h>
static volatile uint8_t *uart = (volatile uint8_t *)(uintptr_t)0x0c170000;
static unsigned int panicking;
void sweet_early_putc(char c)
{
    unsigned int count = 1000000;
    while (!(*(volatile uint32_t *)(uart + 8) & 8))
        if (!--count) return;
    *(volatile uint32_t *)(uart + 0x40) = 1;
    *(volatile uint32_t *)(uart + 0x70) = (uint8_t)c;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void text(const char *s) { while (*s) sweet_early_putc(*s++); }
void sweet_early_mark(unsigned int stage, const char *label)
{
    text("GM9 XNU "); sweet_early_putc('0' + stage / 10);
    sweet_early_putc('0' + stage % 10); text(": "); text(label); text("\r\n");
}
void sweet_early_panic(const char *fmt, va_list *args)
{
    if (panicking) return;
    panicking = 1;
    text("\r\nGM9 XNU PANIC: ");
    va_list copy; va_copy(copy, *args);
    _doprnt(fmt, &copy, sweet_early_putc, 16);
    va_end(copy);
}
void sweet_early_relocate(uint64_t base)
{
    uart = (volatile uint8_t *)(uintptr_t)(base + 0x170000);
    __asm__ volatile("dsb sy" ::: "memory");
    sweet_early_mark(9, "kernel UART mapping live");
}
void sweet_early_disable(void) { }
#endif
'''
(source / 'osfmk/arm64/sweet_early.c').write_text(uart)

def serial(s):
    start = s.index('extern unsigned char iso_font[256 * 16];')
    begin = s.index('{', s.index('fbcon_probe(void)', start))
    end, level = begin + 1, 1
    while level:
        level += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[:start] + '''static void fbcon_init(void) { }
static unsigned int fbcon_transmit_ready(void) { return 1; }
static void fbcon_transmit_data(uint8_t c) { sweet_early_putc((char)c); }
static unsigned int fbcon_receive_ready(void) { return 0; }
static uint8_t fbcon_receive_data(void) { return 0; }
SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) sweet_fbcon_serial_functions = {
    .init = fbcon_init,
    .transmit_ready = fbcon_transmit_ready,
    .transmit_data = fbcon_transmit_data,
    .receive_ready = fbcon_receive_ready,
    .receive_data = fbcon_receive_data,
    .device = SERIAL_SWEET_FBCON
};
static void fbcon_probe(void) { register_serial_functions(&sweet_fbcon_serial_functions); }
''' + s[end:]
update('pexpert/arm/pe_serial.c', serial)

# Use exactly the hardware-tested reader, with XNU's Device mapping instead
# of a physical MMIO pointer. This is a diagnostic entry, not a root device.
probe = (repo / 'shim/gm9pro-probe.c').read_text()
reader = probe[probe.index('/* Reuse ABL'):probe.index('static int64_t cpu_on')]
reader = re.sub(r'\bcrc32\b', 'gm9_crc32', reader)
reader = reader.replace('__asm__ volatile("dsb sy; sev"', '__asm__ volatile("dsb sy\\nsev"')
preamble = r'''#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <stdint.h>
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static uintptr_t regs;
#define EMMC regs
#define RSR(r) ({ uint64_t v; __asm__ volatile("mrs %0, " r : "=r"(v)); v; })
static uint32_t read32(uintptr_t a) { return *(volatile uint32_t *)a; }
static void write32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static void write16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static void write8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static uint64_t ticks(void) { return RSR("cntpct_el0"); }
static uint64_t freq(void) { return RSR("cntfrq_el0"); }
static void puts_uart(const char *s) { printf("%s", s); }
static void hex(const char *s, uint64_t v) { printf("%s=0x%llx\n", s, (unsigned long long)v); }
void gm9pro_emmc_selftest(void);
'''
tail = r'''
void gm9pro_emmc_selftest(void)
{
    regs = ml_io_map(0x0c0c4000, 0x1000);
    if (!regs) panic("GM9 eMMC MMIO mapping failed");
    printf("GM9 eMMC kernel self-test\n");
    emmc_probe();
    panic("GM9 eMMC diagnostic finished; root driver not installed yet");
}
'''
(source / 'bsd/dev/gm9pro_emmc.c').write_text(preamble + reader + tail)
update('bsd/conf/files', lambda s: s + '\nbsd/dev/gm9pro_emmc.c\t\tstandard\n')

def root(s):
    # Run before a root disk is selected, only on an explicit diagnostic boot.
    match = re.search(r'^IOFindBSDRoot\([^{};]*\)\s*\{', s, re.MULTILINE)
    if not match:
        raise RuntimeError('IOFindBSDRoot definition missing')
    begin = match.end() - 1
    return 'extern "C" void gm9pro_emmc_selftest(void);\n' + s[:begin + 1] + '''
    unsigned int gm9test = 0;
    if (PE_parse_boot_argn("gm9emmc", &gm9test, sizeof(gm9test)) && gm9test)
        gm9pro_emmc_selftest();
''' + s[begin + 1:]
update('iokit/bsddev/IOKitBSDInit.cpp', root)
print('GM9 Pro UART, early mapping and eMMC self-test staged')
