#if GM9_DISPLAY_VARIANT == 2
/* Own BSS storage: physically contiguous with the MMU off, no guessed carveout. */
static uint32_t display_test_pixels[1080 * 2160] __attribute__((aligned(4096)));
#endif

/* Read MMCC prerequisites only; raw SMMU reads hung on the previous probe. */
static void display_smmu_snapshot(void)
{
    const uintptr_t mmcc = 0x0c8c0000UL;
    puts_uart("GM9 MMSS clock snapshot begin; SMMU register reads disabled\n");
    /* mmcc-sdm660.c and msm-gdsc-660.dtsi. No clock/power control writes. */
    hex("MMSS SMMU AHB CBCR", read32(mmcc + 0xe004));
    hex("MMSS SMMU AXI CBCR", read32(mmcc + 0xe008));
    hex("MMSS MNOC AHB CBCR", read32(mmcc + 0x5024));
    hex("MMSS SMMU GDSCR", read32(mmcc + 0xe020));
    hex("MMSS SMMU HW CTRL", read32(mmcc + 0xe024));
    puts_uart("GM9 MMSS clock snapshot end; translation unchanged\n");
}

/* Repeated scanout without XNU, using the inherited ABL configuration. */
static void display_probe(void)
{
    const uintptr_t intr = 0x0c901014UL, ctl = 0x0c902000UL;
    const uintptr_t vig = 0x0c905000UL, dsi = 0x0c994000UL;
    const uintptr_t splash = 0x9d400000UL;
    uintptr_t fb = splash;
    const uint32_t done = 1U << 8;
    puts_uart("GM9 DISPLAY REPEAT: MMU-off, 12 frames, no storage commands\n");
    hex("SCTLR_EL1", RSR("sctlr_el1"));
    /* Match the initial proof, but do not hide layout errors with solid gray. */
    if (read32(vig) != 0x08700438U || read32(vig + 0x14) != splash ||
        read32(vig + 0x24) != 4320 || read32(vig + 0x30) != 0x237ff ||
        read32(vig + 0x34) != 0x03020001U) {
        puts_uart("Unexpected VIG0 handoff; no writes made\n");
        return;
    }
#if GM9_DISPLAY_VARIANT == 2
    fb = (uintptr_t)display_test_pixels;
    /* The active ABL DTB is at 0x83200000; keep all test storage below it. */
    if (fb < 0x80008000UL || fb + sizeof(display_test_pixels) > 0x83200000UL) {
        puts_uart("Separate framebuffer outside test image area; stopping\n");
        return;
    }
    puts_uart("VARIANT B: own contiguous framebuffer, VIG0 flush before START\n");
#elif GM9_DISPLAY_VARIANT == 1
    puts_uart("VARIANT A: splash framebuffer, VIG0 flush before START\n");
#endif
    hex("test framebuffer PA", fb);
    if (RSR("sctlr_el1") & 1) {
        puts_uart("MMU unexpectedly enabled; no writes made\n");
        return;
    }
    hex("initial MDP intr", read32(intr));
    hex("initial DSI intr", read32(dsi + 0x110));
    hex("initial DSI FIFO", read32(dsi + 0x0c));
    hex("initial DSI clock", read32(dsi + 0x120));
#if GM9_DISPLAY_VARIANT == 3
    puts_uart("GM9 DISPLAY CONFIG: read-only, no framebuffer or controller writes\n");
    hex("VIG0 source size", read32(vig));
    hex("VIG0 image size", read32(vig + 0x04));
    hex("VIG0 output size", read32(vig + 0x0c));
    hex("VIG0 source address", read32(vig + 0x14));
    hex("VIG0 stride", read32(vig + 0x24));
    hex("VIG0 format", read32(vig + 0x30));
    hex("CTL0 top", read32(ctl + 0x14));
    hex("CTL0 flush", read32(ctl + 0x18));
    hex("DSI control", read32(dsi + 0x04));
    hex("DSI command MDP control", read32(dsi + 0x3c));
    hex("DSI command DCS control", read32(dsi + 0x44));
    hex("DSI stream0 control", read32(dsi + 0x60));
    hex("DSI stream0 total", read32(dsi + 0x64));
    hex("DSI stream1 control", read32(dsi + 0x58));
    hex("DSI stream1 total", read32(dsi + 0x5c));
    hex("DSI trigger control", read32(dsi + 0x84));
    hex("DSI clock control", read32(dsi + 0x11c));
    hex("PP0 sync config", read32(0x0c971004UL));
    hex("PP0 sync height", read32(0x0c971008UL));
    hex("PP0 sync threshold", read32(0x0c971018UL));
    hex("PP0 autorefresh", read32(0x0c971030UL));
    hex("MDP pipe clock control", read32(0x0c9012acUL));
    puts_uart("Controller snapshot complete; reading MMCC prerequisites next\n");
    display_smmu_snapshot();
    puts_uart("DISPLAY CONFIG COMPLETE; reset manually\n");
    return;
#endif
#if GM9_DISPLAY_VARIANT == 0
    display_smmu_snapshot();
#endif
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
#if GM9_DISPLAY_VARIANT != 0
        /* Pixel 0164: latch source, stride and XRGB format through VIG0 flush. */
        write32(vig + 0x14, (uint32_t)fb);
        write32(vig + 0x24, 4320);
        write32(vig + 0x30, 0x000236ffU);
        write32(vig + 0x34, 0x03020001U);
        __asm__ volatile("dsb sy" ::: "memory");
        write32(ctl + 0x18, 1U); /* VIG0 only, no mixer/clock changes. */
        __asm__ volatile("dsb sy" ::: "memory");
#endif
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
#if GM9_DISPLAY_VARIANT == 0
        if (frame == 0 || frame == 11) display_smmu_snapshot();
#endif
        /* Half-second pause, no framebuffer access while scanout may run. */
        deadline = ticks() + freq() / 2;
        while (ticks() < deadline) __asm__ volatile("yield");
    }
    puts_uart("DISPLAY REPEAT COMPLETE; last pattern retained, reset manually\n");
}
