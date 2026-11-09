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
        /* Half-second pause, no framebuffer access while scanout may run. */
        deadline = ticks() + freq() / 2;
        while (ticks() < deadline) __asm__ volatile("yield");
    }
    puts_uart("DISPLAY REPEAT COMPLETE; last pattern retained, reset manually\n");
}
