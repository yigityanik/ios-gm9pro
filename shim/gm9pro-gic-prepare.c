/* Set up NS affinity routing before XNU programs its SGI/PPI frame. */
#include <stdint.h>
#include "console.h"
int gm9pro_prepare_gic(void);
static uint32_t read_ctlr(void)
{ return *(volatile uint32_t *)0x17a00000UL; }
static void write_ctlr(uint32_t value)
{
    *(volatile uint32_t *)0x17a00000UL = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int wait_rwp(void)
{
    uint64_t start, frequency, current;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    do {
        if (!(read_ctlr() & (1U << 31))) return 1;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(current));
    } while (current - start < frequency);
    return 0;
}
int gm9pro_prepare_gic(void)
{
    uint32_t old = read_ctlr();
    con_hex("GICD before affinity routing", old);
    if (!(old & 0x10)) {
        /* Forwarding must be disabled while ARE_NS changes from 0 to 1. */
        write_ctlr(old & ~3U);
        if (!wait_rwp()) return 0;
        write_ctlr((old & ~3U) | 0x10);
        if (!wait_rwp()) return 0;
    }
    uint32_t configured = read_ctlr();
    con_hex("GICD before XNU private IRQ setup", configured);
    if (!(configured & 0x10)) {
        con_str("GIC", "ARE_NS did not take effect");
        return 0;
    }
    con_str("GIC", "affinity routing ready; XNU configures SGIs and PPIs next");
    return 1;
}
