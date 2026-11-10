#define GM9_READ_MAX_BLOCKS 2048U
#include "gm9pro-emmc-multi.inc"
/* Filled before comparison; reserve after the small entry stacks so the
 * entry's PC-relative stack/BSS references stay within their ADR range.
 */
static uint8_t multi_data[GM9_READ_MAX_BLOCKS * 512] __attribute__((section(".probe_buffers")));
static uint8_t single_data[GM9_READ_MAX_BLOCKS * 512] __attribute__((section(".probe_buffers")));
static void
multi_probe(void)
{
    uint8_t ext[512], header[512];
    puts_uart("GM9 EMMC MULTI PROBE: read-only; no filesystem mount\n");
    if (read32(EMMC + 0x24) & 3U) goto fail;
    write32(EMMC + 0x250, read32(EMMC + 0x250) & ~(1U << 10));
    __asm__ volatile("dsb sy" ::: "memory");
    multi_quiet = 1;
    if (read_block(8, 0, ext)) goto fail;
    uint32_t sectors = le32(ext + 212);
    if (ext[192] < 5 || ext[192] > 8 || ext[61] ||
        (ext[179] & 7) || sectors <= 0x400000U) goto fail;
    if (read_block(17, 1, header)) goto fail;
    const char magic[] = "EFI PART";
    for (unsigned i = 0; i < 8; i++)
        if (header[i] != (uint8_t)magic[i]) goto fail;
    uint32_t bytes = le32(header + 12), expected = le32(header + 16);
    if (bytes < 92 || bytes > 512) goto fail;
    for (unsigned i = 16; i < 20; i++) header[i] = 0;
    if (crc32(header, bytes) != expected) goto fail;
    const uint32_t locations[] = { 0, 4096, sectors / 2 };
    const unsigned sizes[] = { 2, 8, 128, 512, 2048 };
    for (unsigned site = 0; site < 3; site++) {
        for (unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
            unsigned blocks = sizes[size];
            uint32_t lba = locations[site];
            if (blocks > sectors - lba) goto fail;
            uint64_t begin = ticks();
            for (unsigned i = 0; i < blocks; i++)
                if (read_block(17, lba + i, single_data + i * 512)) goto fail;
            uint64_t single_ticks = ticks() - begin;
            begin = ticks();
            if (gm9_read_blocks(lba, blocks, multi_data)) goto fail;
            uint64_t multi_ticks = ticks() - begin;
            for (unsigned i = 0; i < blocks * 512; i++)
                if (multi_data[i] != single_data[i]) goto fail;
            /* Follow CMD18 with CMD17, then repeat CMD18: both directions
             * must leave the inherited card in a usable state.
             */
            if (read_block(17, lba, header)) goto fail;
            for (unsigned i = 0; i < 512; i++)
                if (header[i] != single_data[i]) goto fail;
            if (gm9_read_blocks(lba, blocks, multi_data)) goto fail;
            for (unsigned i = 0; i < blocks * 512; i++)
                if (multi_data[i] != single_data[i]) goto fail;
            multi_quiet = 0;
            hex("MULTI PASS LBA", lba);
            hex("blocks", blocks);
            hex("CMD17 microseconds", single_ticks * 1000000 / freq());
            hex("CMD18 microseconds", multi_ticks * 1000000 / freq());
            hex("data CRC32", crc32(multi_data, blocks * 512));
            multi_quiet = 1;
        }
    }
    multi_quiet = 0;
    puts_uart("EMMC MULTI PASS: 15 comparisons through 1 MiB + repeat reads; no sector written\n");
    return;
fail:
    multi_quiet = 0;
    hex("failure interrupt status", read32(EMMC + 0x30));
    hex("failure present state", read32(EMMC + 0x24));
    puts_uart("EMMC MULTI FAIL: stop here and reset manually; no sector written\n");
}
