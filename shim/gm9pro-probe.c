/* SDM660 entry probe. UART must already be configured by ABL.
 * Addresses and CPU affinities come from the GM9 Pro stock device tree.
 * No storage commands or partition writes are issued.
 */
#include <stdint.h>
#define UART 0x0c170000UL
#define EMMC 0x0c0c4000UL
#define RSR(r) ({ uint64_t v; __asm__ volatile("mrs %0, " r : "=r"(v)); v; })
extern void secondary_entry(void);
static volatile struct { uint64_t midr, mmfr0, mpidr, ready; } cpu[8];
static uint32_t read32(uintptr_t a) { return *(volatile uint32_t *)a; }
static void write32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
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
}
void shim_main(uint64_t dtb)
{
    puts_uart("\nGM9PRO SHIM PROBE 1\n");
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
    puts_uart("PROBE COMPLETE; waiting for hardware reset\n");
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
