/* Bounded eMMC PIO reads: CMD23 sets the count; CMD18 consumes it.
 * Caller owns the controller and keeps the inherited card/bus configuration.
 * No DMA mapping, partition selection, card switch or persistent write.
 */
#define GM9_READ_MAX_BLOCKS 128U
static int
gm9_read_blocks(uint32_t lba, unsigned blocks, uint8_t *out)
{
    if (!out || blocks < 2 || blocks > GM9_READ_MAX_BLOCKS ||
        lba > 0xffffffffU - (blocks - 1)) return -1;
    uint64_t deadline = ticks() + freq();
    while (read32(EMMC + 0x24) & 3U)
        if (ticks() >= deadline) return -1;
    uint32_t old_ie = read32(EMMC + 0x34), old_signal = read32(EMMC + 0x38);
    uint8_t old_control = *(volatile uint8_t *)(EMMC + 0x28);
    int rc = -1, data_sent = 0;
    write8(EMMC + 0x28, old_control & ~0x18U);
    write32(EMMC + 0x38, 0);
    write32(EMMC + 0x34, old_ie | 0xffff8033U);
    write32(EMMC + 0x30, 0xffffffffU);
    write16(EMMC + 0x0c, 0);
    write32(EMMC + 0x08, blocks); /* reliable-write and packed-command bits clear */
    __asm__ volatile("dsb sy" ::: "memory");
    write16(EMMC + 0x0e, (23U << 8) | 0x1aU);
    if (wait_status(1)) goto done;
    if (read32(EMMC + 0x10) & 0xffffe080U) goto done;
    write32(EMMC + 0x30, 1);
    deadline = ticks() + freq();
    while (read32(EMMC + 0x24) & 3U)
        if (ticks() >= deadline) goto done;
    write32(EMMC + 0x30, 0xffffffffU);
    write32(EMMC + 0x04, (blocks << 16) | 512U);
    write32(EMMC + 0x08, lba);
    write16(EMMC + 0x0c, 0x32); /* multiblock, read, block-count enabled, PIO */
    __asm__ volatile("dsb sy" ::: "memory");
    if (read32(EMMC + 0x04) != ((blocks << 16) | 512U)) goto done;
    data_sent = 1;
    write16(EMMC + 0x0e, (18U << 8) | 0x3aU);
    if (wait_status(1)) goto done;
    if (read32(EMMC + 0x10) & 0xffffe080U) goto done;
    write32(EMMC + 0x30, 1);
    for (unsigned block = 0; block < blocks; block++) {
        if (wait_status(0x20)) goto done;
        if (!(read32(EMMC + 0x24) & 0x800U)) goto done;
        write32(EMMC + 0x30, 0x20);
        __asm__ volatile("dsb sy" ::: "memory");
        for (unsigned i = 0; i < 512; i += 4) {
            uint32_t word = read32(EMMC + 0x20);
            out[i] = word; out[i + 1] = word >> 8;
            out[i + 2] = word >> 16; out[i + 3] = word >> 24;
        }
        /* Device reads stay ordered; publish the buffer at the block boundary,
         * rather than serializing all memory after each individual FIFO word.
         */
        __asm__ volatile("dsb sy" ::: "memory");
        out += 512;
    }
    if (wait_status(2)) goto done;
    write32(EMMC + 0x30, 2);
    deadline = ticks() + freq();
    while (read32(EMMC + 0x24) & 3U)
        if (ticks() >= deadline) goto done;
    rc = 0;
done:
    if (rc && data_sent) {
        /* CMD12 is an abort: do not wait for DATA_INHIBIT to clear first.
         * The caller must stop using the transport after any failed read.
         */
        deadline = ticks() + freq();
        while ((read32(EMMC + 0x24) & 1U) && ticks() < deadline) { }
        if (!(read32(EMMC + 0x24) & 1U)) {
            write32(EMMC + 0x30, 0xffffffffU);
            write16(EMMC + 0x0c, 0);
            __asm__ volatile("dsb sy" ::: "memory");
            write16(EMMC + 0x0e, (12U << 8) | 0xdbU);
            if (!wait_status(1) && !wait_status(2))
                write32(EMMC + 0x30, 3);
        }
    }
    write32(EMMC + 0x34, old_ie);
    write32(EMMC + 0x38, old_signal);
    write8(EMMC + 0x28, old_control);
    __asm__ volatile("dsb sy" ::: "memory");
    return rc;
}
