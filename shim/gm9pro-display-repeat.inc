/* SDM660 MMSS SMMU: read-only inspection, no translation or fault changes. */
static void display_smmu_snapshot(void)
{
    const uintptr_t base = 0x0cd00000UL;
    puts_uart("GM9 MMSS SMMU snapshot begin\n");
    uint32_t id0 = read32(base + 0x20), id1 = read32(base + 0x24);
    hex("SMMU ID0", id0);
    hex("SMMU ID1", id1);
    hex("SMMU CR0", read32(base));
    hex("SMMU GFSR", read32(base + 0x48));
    hex("SMMU GFSYNR0", read32(base + 0x50));
    hex("SMMU GFSYNR1", read32(base + 0x54));
    unsigned groups = id0 & 255, banks = id1 & 255;
    unsigned shift = (id1 & (1U << 31)) ? 16 : 12;
    if (!groups || groups > 128 || !banks || banks > 32) {
        puts_uart("SMMU identification unavailable/unexpected; no bank reads\n");
        return;
    }
    uint32_t used = 0;
    for (unsigned i = 0; i < groups; i++) {
        uint32_t smr = read32(base + 0x800 + i * 4);
        if (!(smr & (1U << 31))) continue;
        uint32_t s2cr = read32(base + 0xc00 + i * 4);
        hex("SMMU stream index", i);
        hex("SMMU SMR", smr);
        hex("SMMU S2CR", s2cr);
        /* MDP non-secure stream ID is 0 in sdm660-mdss.dtsi. */
        uint32_t mask = (smr >> 16) & 0x7fff;
        unsigned cb = s2cr & 255;
        if (((smr & 0x7fff) & ~mask) == 0 &&
            ((s2cr >> 16) & 3) == 0 && cb < banks) used |= 1U << cb;
    }
    for (unsigned cb = 0; cb < banks; cb++) {
        if (!(used & (1U << cb))) continue;
        uintptr_t off = 0x20000UL + ((uintptr_t)cb << shift);
        /* DTS register window is 0x40000 bytes; never read outside it. */
        if (off + 0x70 > 0x40000UL) continue;
        uintptr_t bank = base + off;
        hex("SMMU MDP candidate CB", cb);
        hex("SMMU CB SCTLR", read32(bank));
        hex("SMMU CB TTBR0 low", read32(bank + 0x20));
        hex("SMMU CB TTBR0 high", read32(bank + 0x24));
        hex("SMMU CB TTBCR", read32(bank + 0x30));
        hex("SMMU CB FSR", read32(bank + 0x58));
        hex("SMMU CB FAR low", read32(bank + 0x60));
        hex("SMMU CB FAR high", read32(bank + 0x64));
        hex("SMMU CB FSYNR0", read32(bank + 0x68));
    }
    puts_uart("GM9 MMSS SMMU snapshot end\n");
}

/* Repeated scanout without XNU, using the inherited ABL configuration. */
static void display_probe(void)
{
    const uintptr_t intr = 0x0c901014UL, ctl = 0x0c902000UL;
    const uintptr_t vig = 0x0c905000UL, dsi = 0x0c994000UL;
    const uintptr_t fb = 0x9d400000UL;
    const uint32_t done = 1U << 8;
    puts_uart("GM9 DISPLAY REPEAT: MMU-off, 12 frames, no storage commands\n");
    hex("SCTLR_EL1", RSR("sctlr_el1"));
    /* Match the initial proof, but do not hide layout errors with solid gray. */
    if (read32(vig) != 0x08700438U || read32(vig + 0x14) != fb ||
        read32(vig + 0x24) != 4320 || read32(vig + 0x30) != 0x237ff ||
        read32(vig + 0x34) != 0x03020001U) {
        puts_uart("Unexpected VIG0 handoff; no writes made\n");
        return;
    }
    if (RSR("sctlr_el1") & 1) {
        puts_uart("MMU unexpectedly enabled; no writes made\n");
        return;
    }
    hex("initial MDP intr", read32(intr));
    hex("initial DSI intr", read32(dsi + 0x110));
    hex("initial DSI FIFO", read32(dsi + 0x0c));
    hex("initial DSI clock", read32(dsi + 0x120));
    display_smmu_snapshot();
    for (unsigned frame = 0; frame < 12; frame++) {
        uint64_t deadline = ticks() + freq() / 10;
        while (((read32(intr) & done) == 0 ||
                (read32(dsi + 0x110) & done) == 0) && ticks() < deadline)
            __asm__ volatile("yield");
        if (!(read32(intr) & done) || !(read32(dsi + 0x110) & done)) {
            puts_uart("Completion missing; no further framebuffer writes\n");
            break;
        }
        volatile uint32_t *pixels = (volatile uint32_t *)fb;
        static const uint32_t bars[] = {
            0x00ff0000U, 0x0000ff00U, 0x000000ffU,
            0x00ffffffU, 0x00808080U, 0x00000000U
        };
        for (unsigned y = 0; y < 2160; y++) {
            for (unsigned x = 0; x < 1080; x++) {
                unsigned band = ((x / 180) + frame) % 6;
                uint32_t color = bars[band];
                /* Coordinate grid exposes line/stride errors. */
                if (x % 180 < 3 || y % 180 < 3) color = 0x00ffffffU;
                pixels[y * 1080 + x] = color;
            }
        }
        __asm__ volatile("dsb sy" ::: "memory");
        /* Match the XNU test: clear completion only, preserve error latches. */
        write32(dsi + 0x110, (read32(dsi + 0x110) & 0xa2220202U) | done);
        write32(intr + 4, done);
        __asm__ volatile("dsb sy" ::: "memory");
        if ((read32(intr) & done) || (read32(dsi + 0x110) & done)) {
            puts_uart("Completion did not clear; no further kickoffs\n");
            break;
        }
        write32(ctl + 0x1c, 1);
        __asm__ volatile("dsb sy" ::: "memory");
        deadline = ticks() + freq() / 10;
        while (((read32(intr) & done) == 0 ||
                (read32(dsi + 0x110) & done) == 0) && ticks() < deadline)
            __asm__ volatile("yield");
        hex("frame", frame + 1);
        hex("MDP intr", read32(intr));
        hex("DSI intr", read32(dsi + 0x110));
        hex("DSI status", read32(dsi + 0x08));
        hex("DSI FIFO", read32(dsi + 0x0c));
        hex("DSI clock", read32(dsi + 0x120));
        hex("DSI lane", read32(dsi + 0xa8));
        if (frame == 0 || frame == 11) display_smmu_snapshot();
        /* Half-second pause, no framebuffer access while scanout may run. */
        deadline = ticks() + freq() / 2;
        while (ticks() < deadline) __asm__ volatile("yield");
    }
    puts_uart("DISPLAY REPEAT COMPLETE; last pattern retained, reset manually\n");
}
