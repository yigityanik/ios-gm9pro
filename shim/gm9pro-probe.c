/* SDM660 entry probe. UART must already be configured by ABL.
 * Addresses and CPU affinities come from the GM9 Pro stock device tree.
 * Only EXT_CSD and single-block reads are issued to storage.
 */
#include <stdint.h>
#ifdef GM9PRO_XNU_LOADER
#include "loader.h"
extern char _end[];
#endif
#define UART 0x0c170000UL
#define EMMC 0x0c0c4000UL
#define RSR(r) ({ uint64_t v; __asm__ volatile("mrs %0, " r : "=r"(v)); v; })
extern void secondary_entry(void);
static volatile struct { uint64_t midr, mmfr0, mpidr, ready; } cpu[8];
static volatile uint64_t run_big;
static uint32_t read32(uintptr_t a) { return *(volatile uint32_t *)a; }
static void write32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static void write16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static void write8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static uint64_t ticks(void) { return RSR("cntpct_el0"); }
static uint64_t freq(void) { return RSR("cntfrq_el0"); }
static void putc_uart(char c)
{
    uint64_t end = ticks() + freq() / 10;
    while (!(read32(UART + 0x08) & (1U << 3)))
        if (ticks() >= end) return;
    write32(UART + 0x40, 1); /* UARTDM_NCF_TX */
    write32(UART + 0x70, (unsigned char)c);
    __asm__ volatile("dsb sy" ::: "memory");
}
static void puts_uart(const char *s)
{
    while (*s) { if (*s == '\n') putc_uart('\r'); putc_uart(*s++); }
}
static void hex(const char *name, uint64_t value)
{
    puts_uart(name); puts_uart("=0x");
    for (int i = 60; i >= 0; i -= 4)
        putc_uart("0123456789abcdef"[(value >> i) & 15]);
    puts_uart("\n");
}
/* Reuse ABL's selected card, voltage, clock, DLL and bus-width setup.
 * Poll the SDHCI PIO path; do not reinitialize the card or select a partition.
 */
static int wait_status(uint32_t wanted)
{
    uint64_t end = ticks() + 2 * freq();
    do {
        uint32_t status = read32(EMMC + 0x30);
        if (status & 0xffff8000U) {
            hex("SDHCI error status", status);
            return -1;
        }
        if ((status & wanted) == wanted) return 0;
    } while (ticks() < end);
    hex("SDHCI timeout waiting", wanted);
    hex("SDHCI status", read32(EMMC + 0x30));
    hex("SDHCI present", read32(EMMC + 0x24));
    return -1;
}
static int read_block(unsigned cmd, uint32_t arg, uint8_t *out)
{
    /* Keep this interface incapable of issuing write/erase/switch commands. */
    if (cmd != 8 && cmd != 17) return -1;
    uint64_t end = ticks() + freq();
    while (read32(EMMC + 0x24) & 3) {
        if (ticks() >= end) { puts_uart("SDHCI busy before command\n"); return -1; }
    }
    uint32_t old_enable = read32(EMMC + 0x34);
    uint32_t old_signal = read32(EMMC + 0x38);
    uint8_t old_control = *(volatile uint8_t *)(EMMC + 0x28);
    /* Match Linux sdhci_config_dma(): ADMA selection can break PIO even
     * when the transfer-mode DMA-enable bit is clear. Preserve bus width
     * and the neighboring power-control byte.
     */
    write8(EMMC + 0x28, old_control & ~0x18U);
    __asm__ volatile("dsb sy" ::: "memory");
    write32(EMMC + 0x38, 0);
    write32(EMMC + 0x34, old_enable | 0xffff8033U);
    write32(EMMC + 0x30, 0xffffffffU);
    /* Program block size/count together, and verify the bus write landed. */
    write32(EMMC + 0x04, (1U << 16) | 512);
    write32(EMMC + 0x08, arg);
    write16(EMMC + 0x0c, 0x12); /* one block, count enabled, read, PIO */
    __asm__ volatile("dsb sy" ::: "memory");
    hex("SDHCI block size/count readback", read32(EMMC + 0x04));
    hex("SDHCI transfer mode readback", *(volatile uint16_t *)(EMMC + 0x0c));
    hex("SDHCI host control", read32(EMMC + 0x28));
    hex("SDHCI host control2", *(volatile uint16_t *)(EMMC + 0x3e));
    if (read32(EMMC + 0x04) != ((1U << 16) | 512)) {
        puts_uart("Block configuration mismatch; command not sent\n");
        write32(EMMC + 0x34, old_enable);
        write32(EMMC + 0x38, old_signal);
        write8(EMMC + 0x28, old_control);
        return -1;
    }
    write16(EMMC + 0x0e, (cmd << 8) | 0x3a); /* R1, CRC, index, data */
    int rc = -1;
    if (wait_status(1)) goto done;
    uint32_t r1 = read32(EMMC + 0x10);
    hex("eMMC R1", r1);
    if (r1 & 0xffffe080U) { puts_uart("eMMC rejected command\n"); goto done; }
    write32(EMMC + 0x30, 1);
    if (wait_status(0x20)) goto done;
    if (!(read32(EMMC + 0x24) & 0x800)) {
        puts_uart("SDHCI FIFO not readable\n"); goto done;
    }
    /* Acknowledge buffer-ready before draining, as in the SDHCI IRQ path. */
    write32(EMMC + 0x30, 0x20);
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < 512; i += 4) {
        uint32_t v = read32(EMMC + 0x20);
        __asm__ volatile("dsb sy" ::: "memory");
        out[i] = v; out[i + 1] = v >> 8;
        out[i + 2] = v >> 16; out[i + 3] = v >> 24;
    }
    hex("SDHCI block size/count after FIFO", read32(EMMC + 0x04));
    hex("SDHCI present after FIFO", read32(EMMC + 0x24));
    hex("FIFO first word", (uint32_t)out[0] | (uint32_t)out[1] << 8 |
        (uint32_t)out[2] << 16 | (uint32_t)out[3] << 24);
    if (cmd == 8) {
        hex("EXT_CSD raw revision", out[192]);
        hex("EXT_CSD raw sectors", (uint32_t)out[212] | (uint32_t)out[213] << 8 |
            (uint32_t)out[214] << 16 | (uint32_t)out[215] << 24);
    }
    if (wait_status(2)) goto done;
    write32(EMMC + 0x30, 2);
    rc = 0;
done:
    write32(EMMC + 0x34, old_enable);
    write32(EMMC + 0x38, old_signal);
    write8(EMMC + 0x28, old_control);
    return rc;
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t crc32(const uint8_t *p, unsigned len)
{
    uint32_t c = ~0U;
    while (len--) {
        c ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1)));
    }
    return ~c;
}
static void emmc_probe(void)
{
    uint8_t ext[512], sector[512], again[512];
    /* SDCC v5: sdhci-msm's CORE_VENDOR_SPEC3 is at 0x250. Linux clears
     * CORE_FIFO_ALT_EN (bit 10) to enable the standard SDHCI buffer port.
     * Do this only while both command and data engines are idle.
     */
    if (read32(EMMC + 0x24) & 3) {
        puts_uart("eMMC busy before FIFO selection; stopping\n"); return;
    }
    uint32_t vendor3 = read32(EMMC + 0x250);
    hex("SDCC vendor3 before", vendor3);
    write32(EMMC + 0x250, vendor3 & ~(1U << 10));
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t selected = read32(EMMC + 0x250);
    hex("SDCC vendor3 after", selected);
    if (selected != (vendor3 & ~(1U << 10))) {
        puts_uart("SDHCI FIFO selection readback mismatch; stopping\n"); return;
    }
    puts_uart("eMMC CMD8 EXT_CSD\n");
    if (read_block(8, 0, ext)) return;
    hex("EXT_CSD revision", ext[192]);
    hex("EXT_CSD partition config", ext[179]);
    hex("EXT_CSD bus width", ext[183]);
    hex("EXT_CSD HS timing", ext[185]);
    uint32_t count = le32(ext + 212);
    hex("eMMC sectors", count);
    if (ext[192] < 5 || ext[192] > 8 || count <= 0x400000 ||
        ext[61] != 0 || (ext[179] & 7)) {
        puts_uart("Unsupported EXT_CSD or non-user partition; stopping\n"); return;
    }
    puts_uart("eMMC CMD17 LBA 0\n");
    if (read_block(17, 0, sector)) return;
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        puts_uart("MBR signature missing\n"); return;
    }
    puts_uart("MBR signature OK; eMMC CMD17 LBA 1\n");
    if (read_block(17, 1, sector)) return;
    const char magic[] = "EFI PART";
    for (unsigned i = 0; i < 8; i++)
        if (sector[i] != (uint8_t)magic[i]) { puts_uart("GPT signature missing\n"); return; }
    uint32_t size = le32(sector + 12), expected = le32(sector + 16);
    if (size < 92 || size > 512 || le32(sector + 24) != 1 || le32(sector + 28)) {
        puts_uart("Invalid GPT header\n"); return;
    }
    for (unsigned i = 16; i < 20; i++) sector[i] = 0;
    uint32_t actual = crc32(sector, size);
    hex("GPT header CRC expected", expected);
    hex("GPT header CRC actual", actual);
    if (actual != expected) { puts_uart("GPT header CRC mismatch\n"); return; }
    puts_uart("Repeat LBA 1 read\n");
    if (read_block(17, 1, again)) return;
    for (unsigned i = 16; i < 20; i++) sector[i] = again[i];
    if (le32(again + 16) != expected) { puts_uart("GPT CRC changed\n"); return; }
    for (unsigned i = 0; i < 512; i++)
        if (sector[i] != again[i]) { puts_uart("Repeated sector read differs\n"); return; }
    puts_uart("EMMC READ PASS: MBR, GPT header CRC and repeat read verified\n");
}
static int64_t cpu_on(uint64_t target, uint64_t idx)
{
    register uint64_t x0 __asm__("x0") = 0xc4000003;
    register uint64_t x1 __asm__("x1") = target;
    register uint64_t x2 __asm__("x2") = (uintptr_t)secondary_entry;
    register uint64_t x3 __asm__("x3") = idx;
    __asm__ volatile("dsb sy; smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "x4", "x5", "x6", "x7", "memory");
    return (int64_t)x0;
}
void secondary_main(uint64_t idx)
{
    if (idx >= 8) return;
    cpu[idx].midr = RSR("midr_el1");
    cpu[idx].mmfr0 = RSR("id_aa64mmfr0_el1");
    cpu[idx].mpidr = RSR("mpidr_el1");
    __asm__ volatile("dsb sy" ::: "memory");
    cpu[idx].ready = 1;
    if (idx == 4) {
        while (!run_big) __asm__ volatile("yield");
        __asm__ volatile("dsb sy" ::: "memory");
        puts_uart("BIG CORE HANDOFF\n");
        hex("executing MPIDR", RSR("mpidr_el1"));
        hex("executing TGran16", (RSR("id_aa64mmfr0_el1") >> 20) & 15);
#ifdef GM9PRO_XNU_LOADER
        const char *error = loader_boot_rd((uintptr_t)_end,
            (uintptr_t)_end + PAYLOAD_BYTES);
        puts_uart("XNU loader returned: "); puts_uart(error); puts_uart("\n");
#else
        emmc_probe();
#endif
        puts_uart("PROBE 5 COMPLETE; waiting for hardware reset\n");
        for (;;) __asm__ volatile("wfe");
    }
}
void shim_main(uint64_t dtb)
{
    puts_uart("\nGM9PRO SHIM PROBE 5\n");
    hex("CurrentEL", RSR("CurrentEL"));
    hex("DTB", dtb);
    hex("SCTLR_EL1", RSR("sctlr_el1"));
    hex("MIDR_EL1", RSR("midr_el1"));
    hex("MPIDR_EL1", RSR("mpidr_el1"));
    hex("ID_AA64MMFR0_EL1", RSR("id_aa64mmfr0_el1"));
    uint64_t boot_aff = RSR("mpidr_el1") & 0xffffff;
    for (unsigned i = 0; i < 8; i++) {
        uint64_t aff = i < 4 ? i : 0x100 + i - 4;
        if (aff == boot_aff) continue;
        hex("CPU target", aff);
        int64_t rc = cpu_on(aff, i);
        hex("PSCI CPU_ON", rc);
        if (rc) continue;
        uint64_t end = ticks() + freq();
        while (!cpu[i].ready && ticks() < end) __asm__ volatile("yield");
        if (!cpu[i].ready) { puts_uart("CPU response timeout\n"); continue; }
        __asm__ volatile("dsb sy" ::: "memory");
        hex("CPU MIDR", cpu[i].midr);
        hex("CPU MPIDR", cpu[i].mpidr);
        hex("CPU MMFR0", cpu[i].mmfr0);
        hex("CPU TGran16", (cpu[i].mmfr0 >> 20) & 15);
    }
    puts_uart("eMMC controller read-only inspection\n");
    hex("SDHCI present state", read32(EMMC + 0x24));
    hex("SDHCI capabilities", read32(EMMC + 0x40));
    hex("SDHCI clock/timeout/reset", read32(EMMC + 0x2c));
    hex("SDHCI host version", *(volatile uint16_t *)(EMMC + 0xfe));
    if (!cpu[4].ready || ((cpu[4].mmfr0 >> 20) & 15) == 0) {
        puts_uart("Big CPU unavailable; stopping\n");
        for (;;) __asm__ volatile("wfe");
    }
    puts_uart("Releasing big CPU 0x100; parking boot CPU\n");
    run_big = 1;
    __asm__ volatile("dsb sy; sev" ::: "memory");
    for (;;) __asm__ volatile("wfe");
}
void exc_handler(uint64_t slot, uint64_t *regs)
{
    (void)regs;
    puts_uart("GM9PRO PROBE EXCEPTION\n");
    hex("vector", slot); hex("ESR_EL1", RSR("esr_el1"));
    hex("FAR_EL1", RSR("far_el1")); hex("ELR_EL1", RSR("elr_el1"));
    for (;;) __asm__ volatile("wfe");
}
