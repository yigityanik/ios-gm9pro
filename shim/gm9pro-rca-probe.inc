/* Read-only CMD13 diagnostic: it never sends data-write or card-switch commands. */
static int rp_reset_command_line(void)
{
    write8(EMMC + 0x2f, 0x02); /* SDHCI_RESET_CMD */
    uint64_t end = ticks() + freq();
    while (*(volatile uint8_t *)(EMMC + 0x2f) & 0x02)
        if (ticks() >= end) return -1;
    write32(EMMC + 0x30, 0xffffffffU);
    return 0;
}

static int rp_status(unsigned rca, int clear_mode)
{
    uint64_t end = ticks() + 2 * freq();
    while (read32(EMMC + 0x24) & 3)
        if (ticks() >= end) { puts_uart("Controller inhibit before CMD13\n"); return -1; }
    write32(EMMC + 0x30, 0xffffffffU);
    if (clear_mode) write16(EMMC + 0x0c, 0);
    write32(EMMC + 0x08, (uint32_t)rca << 16);
    __asm__ volatile("dsb sy" ::: "memory");
    hex("CMD13 RCA", rca);
    hex("CMD13 transfer mode", *(volatile uint16_t *)(EMMC + 0x0c));
    write16(EMMC + 0x0e, (13U << 8) | 0x1a); /* R1, CRC and index */
    end = ticks() + 2 * freq();
    uint32_t status;
    do {
        status = read32(EMMC + 0x30);
        if ((status & 0xffff8000U) || (status & 1U)) break;
    } while (ticks() < end);
    hex("CMD13 interrupt status", status);
    if ((status & 0xffff8000U) || !(status & 1U)) {
        hex("CMD13 present state on failure", read32(EMMC + 0x24));
        if (rp_reset_command_line()) { puts_uart("Command-line reset failed\n"); return -1; }
        return 0;
    }
    uint32_t r1 = read32(EMMC + 0x10);
    write32(EMMC + 0x30, 1);
    hex("CMD13 response R1", r1);
    if (!(r1 & 0xffffe080U) && (r1 & 0x1f00U) == 0x900U) {
        puts_uart("CMD13 READY: matching RCA and transfer state\n");
        return 1;
    }
    return 0;
}

static void gm9_rca_probe(void)
{
    static const unsigned candidates[] = {1, 0, 2, 3, 4, 0x10, 0xff, 0x100, 0x1234, 0xffff};
    uint32_t old_ie = read32(EMMC + 0x34), old_signal = read32(EMMC + 0x38);
    uint16_t old_mode = *(volatile uint16_t *)(EMMC + 0x0c);
    uint8_t block[512];
    puts_uart("GM9 RCA PROBE: CMD13 status queries only\n");
    write32(EMMC + 0x38, 0);
    write32(EMMC + 0x34, old_ie | 0xffff8001U);
    int result = rp_status(1, 0); /* Repeat the original register sequence. */
    if (result == 0) {
        for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            result = rp_status(candidates[i], 1);
            if (result != 0) break;
        }
    }
    if (result == 0) puts_uart("CMD13: none of the tested RCA values responded\n");
    else if (result < 0) puts_uart("CMD13: controller recovery failed; stopping\n");
    write16(EMMC + 0x0c, old_mode);
    write32(EMMC + 0x34, old_ie);
    write32(EMMC + 0x38, old_signal);
    if (result >= 0) {
        if (!read_block(17, 0, block) && block[510] == 0x55 && block[511] == 0xaa)
            puts_uart("RCA PROBE: subsequent MBR read PASS\n");
        else puts_uart("RCA PROBE: subsequent MBR read FAILED\n");
    }
    puts_uart("RCA PROBE COMPLETE; no eMMC sector writes sent\n");
}
