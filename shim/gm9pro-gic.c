/* GICv3 self-SGI diagnostic on the GM9 Pro big boot CPU. */
#include <stdint.h>
#include "console.h"
#define GICD 0x17a00000UL
#define GICR 0x17b00000UL
#define GET(r) ({ uint64_t v; __asm__ volatile("mrs %0, " r : "=r"(v)); v; })
#define SET(r,v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)) : "memory")
extern void secondary_entry(void);
static volatile uint64_t release_big, hits, last_irq;
static volatile uint64_t irq_mask;
static uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static void wr(uintptr_t a, uint32_t v)
{ *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }
static uint64_t now(void) { return GET("cntpct_el0"); }
static uintptr_t frame(void)
{
    uint64_t affinity = GET("mpidr_el1") & 0xffffff;
    for (uintptr_t off = 0; off < 0x100000; off += 0x20000) {
        uint64_t typer = *(volatile uint64_t *)(GICR + off + 8);
        if (((typer >> 32) & 0xffffff) == affinity) return GICR + off;
        if (typer & 16) break;
    }
    return 0;
}
static void dump(uintptr_t r)
{
    con_hex("GICD_CTLR", rd(GICD));
    con_hex("GICR frame", r);
    con_hex("GICR_WAKER", rd(r + 0x14));
    con_hex("GICR_IGROUPR0", rd(r + 0x10080));
    con_hex("GICR_IGRPMODR0", rd(r + 0x10d00));
    con_hex("GICR_ISENABLER0", rd(r + 0x10100));
    con_hex("GICR_ISPENDR0", rd(r + 0x10200));
    con_hex("GICR_ISACTIVER0", rd(r + 0x10300));
    con_hex("GICR_IPRIORITYR0", rd(r + 0x10400));
    con_hex("ICC_SRE_EL1", GET("icc_sre_el1"));
    con_hex("ICC_CTLR_EL1", GET("icc_ctlr_el1"));
    con_hex("ICC_IGRPEN1_EL1", GET("icc_igrpen1_el1"));
    con_hex("ICC_PMR_EL1", GET("icc_pmr_el1"));
    con_hex("ICC_RPR_EL1", GET("icc_rpr_el1"));
    con_hex("ICC_HPPIR1_EL1", GET("icc_hppir1_el1"));
    con_hex("DAIF", GET("daif"));
}
void secondary_main(uint64_t index)
{
    (void)index;
    while (!release_big) __asm__ volatile("yield");
    __asm__ volatile("dsb sy" ::: "memory");
    con_hex("BIG MPIDR", GET("mpidr_el1"));
    uintptr_t r = frame();
    if (!r) { con_str("GIC", "redistributor not found"); return; }
    con_str("GIC", "before configuration"); dump(r);
    uint32_t waker = rd(r + 0x14);
    wr(r + 0x14, waker & ~2U);
    uint64_t end = now() + GET("cntfrq_el0");
    while (rd(r + 0x14) & 4)
        if (now() >= end) { con_str("GIC", "redistributor wake timeout"); return; }
    wr(r + 0x10180, 0xffffffffU); /* mask inherited private interrupts */
    wr(r + 0x10080, 0xffffffffU);
    for (unsigned i = 0; i < 16; i += 4) wr(r + 0x10400 + i, 0xa0a0a0a0);
    wr(r + 0x10280, 2); /* clear pending SGI1 */
    wr(r + 0x10380, 2); /* clear stale active SGI1 */
    wr(r + 0x10100, 0xffff);
    wr(GICD, rd(GICD) | 0x13);
    end = now() + GET("cntfrq_el0");
    while (rd(GICD) & (1U << 31))
        if (now() >= end) { con_str("GIC", "distributor RWP timeout"); return; }
    SET("icc_sre_el1", GET("icc_sre_el1") | 1);
    __asm__ volatile("isb" ::: "memory");
    SET("icc_bpr1_el1", 0);
    SET("icc_pmr_el1", 0xff);
    SET("icc_ctlr_el1", GET("icc_ctlr_el1") & ~2ULL);
    SET("icc_igrpen1_el1", 1);
    __asm__ volatile("dsb sy; isb" ::: "memory");
    con_str("GIC", "configured, IRQ still masked"); dump(r);
    uint64_t mpidr = GET("mpidr_el1");
    uint64_t sgi = (((mpidr >> 32) & 255) << 48) |
        (((mpidr >> 16) & 255) << 32) | (((mpidr >> 8) & 255) << 16) |
        (1ULL << 24) | (1ULL << (mpidr & 255));
    con_hex("SGI1 target encoding", sgi);
    SET("icc_sgi1r_el1", sgi);
    __asm__ volatile("dsb sy; isb" ::: "memory");
    con_str("GIC", "after send, before IRQ unmask"); dump(r);
    end = now() + GET("cntfrq_el0");
    __asm__ volatile("msr daifclr, #2; isb" ::: "memory");
    while (!hits && now() < end) __asm__ volatile("yield");
    __asm__ volatile("msr daifset, #2; isb" ::: "memory");
    con_hex("SGI1 IRQ hits", hits);
    con_hex("last IRQ", last_irq);
    dump(r);
    if (!hits) {
        uint64_t iar = GET("icc_iar1_el1");
        con_hex("manual IAR1", iar);
        if ((iar & 0xffffff) < 1020) SET("icc_eoir1_el1", iar);
    }
    con_str("GIC TEST", hits ? "IRQ DELIVERY PASS" : "NO IRQ DELIVERY");
    uint64_t sent = sgi & ~(15ULL << 24);
    for (unsigned id = 0; id < 16; id++) {
        uint64_t before = irq_mask;
        SET("icc_sgi1r_el1", sent | ((uint64_t)id << 24));
        __asm__ volatile("dsb sy; isb" ::: "memory");
        end = now() + GET("cntfrq_el0") / 100;
        __asm__ volatile("msr daifclr, #2; isb" ::: "memory");
        while (irq_mask == before && now() < end) __asm__ volatile("yield");
        __asm__ volatile("msr daifset, #2; isb" ::: "memory");
        uint64_t iar = GET("icc_iar1_el1");
        if ((iar & 0xffffff) < 1020) {
            con_hex("sweep manually acknowledged INTID", iar);
            SET("icc_eoir1_el1", iar);
        }
    }
    con_hex("SGI sweep IRQ mask", irq_mask);
    con_hex("SGI enable mask after sweep", rd(r + 0x10100));
    con_str("GIC TEST", "SGI sweep complete");
    for (;;) __asm__ volatile("wfe");
}
void shim_main(uint64_t dtb)
{
    con_str("GM9PRO", "GIC SGI diagnostic 2"); con_hex("DTB", dtb);
    con_hex("primary MPIDR", GET("mpidr_el1"));
    uintptr_t primary = frame();
    if (!primary) { con_str("GIC", "primary frame missing"); return; }
    con_str("GIC", "primary access before configuration"); dump(primary);
    wr(primary + 0x10080, 0xffffffffU);
    wr(primary + 0x10100, 0xffff);
    wr(primary + 0x10400, 0xa0a0a0a0);
    wr(GICD, rd(GICD) | 0x13);
    con_str("GIC", "primary access after configuration"); dump(primary);
    /* Compare access to the target frame from the original boot CPU. */
    for (uintptr_t off = 0; off < 0x100000; off += 0x20000) {
        uintptr_t remote = GICR + off;
        uint64_t typer = *(volatile uint64_t *)(remote + 8);
        if (((typer >> 32) & 0xffffff) == 0x100) {
            wr(remote + 0x14, rd(remote + 0x14) & ~2U);
            wr(remote + 0x10080, 0xffffffffU);
            wr(remote + 0x10100, 0xffff);
            wr(remote + 0x10400, 0xa0a0a0a0);
            con_str("GIC", "target frame configured by primary"); dump(remote);
            break;
        }
        if (typer & 16) break;
    }
    register uint64_t x0 __asm__("x0") = 0xc4000003;
    register uint64_t x1 __asm__("x1") = 0x100;
    register uint64_t x2 __asm__("x2") = (uintptr_t)secondary_entry;
    register uint64_t x3 __asm__("x3") = 4;
    __asm__ volatile("dsb sy; smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : : "x4", "x5", "x6", "x7", "memory");
    con_hex("PSCI CPU_ON", x0);
    if (!x0) { release_big = 1; __asm__ volatile("dsb sy; sev" ::: "memory"); }
    for (;;) __asm__ volatile("wfe");
}
void exc_handler(uint64_t slot, uint64_t *regs)
{
    (void)regs;
    if (slot == 5) {
        uint64_t iar = GET("icc_iar1_el1");
        unsigned int id = iar & 0xffffff;
        last_irq = id;
        if (id < 16) irq_mask |= 1ULL << id;
        if (id == 1) hits++;
        if (id < 1020) {
            if (id >= 32) wr(GICD + 0x180 + (id / 32) * 4, 1U << (id % 32));
            SET("icc_eoir1_el1", iar);
            __asm__ volatile("isb" ::: "memory");
        }
        return;
    }
    con_hex("GIC exception slot", slot); con_hex("ESR", GET("esr_el1"));
    con_hex("ELR", GET("elr_el1")); con_hex("FAR", GET("far_el1"));
    for (;;) __asm__ volatile("wfe");
}
