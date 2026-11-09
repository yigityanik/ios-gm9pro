/* mdss_mdp_cmd_tearcheck_setup: stop external TE, drain, disable, restore TE. */
static int display_manual_takeover(uintptr_t intr)
{
    const uintptr_t pp = 0x0c971000UL;
    uint32_t auto_cfg = read32(pp + 0x30);
    uint32_t sync = read32(pp + 0x04);
    hex("takeover initial autorefresh", auto_cfg);
    hex("takeover initial sync", sync);
    if (!(auto_cfg & (1U << 31))) return 1;
    write32(pp + 0x04, sync & ~(1U << 20));
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t line = read32(pp + 0x2c);
    hex("takeover line count", line);
    if (line && line < 2160) {
        /* Discard a stale completion before waiting for the active frame. */
        write32(intr + 4, 1U << 8);
        __asm__ volatile("dsb sy" ::: "memory");
        uint64_t deadline = ticks() + freq() / 10;
        while (!(read32(intr) & (1U << 8)) && ticks() < deadline)
            __asm__ volatile("yield");
        line = read32(pp + 0x2c);
        if (!(read32(intr) & (1U << 8)) && line != 2160) {
            write32(pp + 0x04, sync);
            __asm__ volatile("dsb sy" ::: "memory");
            hex("takeover timeout line count", line);
            puts_uart("Autorefresh drain failed; TE restored, no software START issued\n");
            return 0;
        }
    }
    write32(pp + 0x30, 0);
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t after = read32(pp + 0x30);
    write32(pp + 0x04, sync);
    __asm__ volatile("dsb sy" ::: "memory");
    hex("takeover final autorefresh", after);
    hex("takeover final sync", read32(pp + 0x04));
    if (after & (1U << 31)) {
        puts_uart("Autorefresh stayed enabled; no software START issued\n");
        return 0;
    }
    puts_uart("AUTOREFRESH TAKEOVER COMPLETE\n");
    return 1;
}

