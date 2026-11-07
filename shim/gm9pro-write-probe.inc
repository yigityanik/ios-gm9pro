/* One-sector userdata PIO diagnostic. No filesystem or GPT sector is written. */
static uint64_t wp_first[256], wp_last[256];
static uint8_t wp_ext[512], wp_buf[512], wp_original[512], wp_pattern[512];

static uint64_t wp_le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static uint32_t wp_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
        (uint32_t)p[2] << 8 | p[3];
}
static uint32_t wp_crc_update(uint32_t crc, const uint8_t *p, unsigned n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return crc;
}

/* RCA 2 responded in transfer state on the GM9 Pro diagnostic probe. */
static int wp_card_ready(void)
{
    uint64_t deadline = ticks() + 5 * freq();
    while (ticks() < deadline) {
        if (read32(EMMC + 0x24) & 3) continue;
        write32(EMMC + 0x30, 0xffffffffU);
        write16(EMMC + 0x0c, 0);
        write32(EMMC + 0x08, 2U << 16);
        write16(EMMC + 0x0e, (13U << 8) | 0x1a);
        if (wait_status(1)) return -1;
        uint32_t r1 = read32(EMMC + 0x10);
        write32(EMMC + 0x30, 1);
        if (r1 & 0xffffe080U) { hex("CMD13 error R1", r1); return -1; }
        if ((r1 & 0x1f00U) == 0x900U) return 0;
    }
    puts_uart("CMD13 timed out waiting for card transfer state\n");
    return -1;
}

/* Single-block CMD24 PIO; caller has already checked the only allowed LBA. */
static int wp_write_one(uint32_t lba, const uint8_t *data, int *command_sent)
{
    uint64_t deadline = ticks() + 5 * freq();
    while (read32(EMMC + 0x24) & 3)
        if (ticks() >= deadline) { puts_uart("SDHCI busy before write\n"); return -1; }
    uint32_t old_ie = read32(EMMC + 0x34), old_signal = read32(EMMC + 0x38);
    uint8_t old_control = *(volatile uint8_t *)(EMMC + 0x28);
    int result = -1;
    write8(EMMC + 0x28, old_control & ~0x18U);
    write32(EMMC + 0x38, 0);
    write32(EMMC + 0x34, old_ie | 0xffff8033U);
    if (wp_card_ready()) goto out;
    write32(EMMC + 0x30, 0xffffffffU);
    write32(EMMC + 0x04, (1U << 16) | 512);
    write32(EMMC + 0x08, lba);
    write16(EMMC + 0x0c, 0x02); /* one block, PIO, write */
    __asm__ volatile("dsb sy" ::: "memory");
    if (read32(EMMC + 0x04) != ((1U << 16) | 512)) goto out;
    *command_sent = 1;
    write16(EMMC + 0x0e, (24U << 8) | 0x3a);
    if (wait_status(1)) goto out;
    uint32_t r1 = read32(EMMC + 0x10);
    hex("CMD24 R1", r1);
    if (r1 & 0xffffe080U) goto out;
    write32(EMMC + 0x30, 1);
    if (wait_status(0x10)) goto out;
    if (!(read32(EMMC + 0x24) & 0x400U)) goto out;
    write32(EMMC + 0x30, 0x10);
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < 512; i += 4) {
        uint32_t word = (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 |
            (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24;
        write32(EMMC + 0x20, word);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    if (wait_status(2)) goto out;
    write32(EMMC + 0x30, 2);
    if (wp_card_ready()) goto out;
    result = 0;
out:
    if (result) {
        hex("SDHCI write interrupt status", read32(EMMC + 0x30));
        hex("SDHCI write present state", read32(EMMC + 0x24));
    }
    write32(EMMC + 0x34, old_ie);
    write32(EMMC + 0x38, old_signal);
    write8(EMMC + 0x28, old_control);
    return result;
}

static int wp_userdata_name(const uint8_t *entry)
{
    static const char name[] = "userdata";
    for (unsigned i = 0; i < sizeof(name); i++)
        if (entry[56 + 2 * i] != (uint8_t)name[i] || entry[57 + 2 * i]) return 0;
    return 1;
}

static void gm9_write_probe(void)
{
    puts_uart("GM9 EMMC WRITE PROBE: checking safety conditions\n");
    if (read_block(8, 0, wp_ext)) return;
    hex("CACHE_CTRL", wp_ext[33]); hex("CMDQ_MODE_EN", wp_ext[15]);
    hex("USER_WP", wp_ext[171]);
    if ((wp_ext[33] & 1) || wp_ext[15] || wp_ext[171] || (wp_ext[179] & 7) ||
        wp_ext[61] || wp_ext[192] < 5 || wp_ext[192] > 8) {
        puts_uart("WRITE PROBE REFUSED: unsupported cache, queue, protection or card state\n"); return;
    }
    uint64_t card_sectors = le32(wp_ext + 212);
    if (read_block(17, 1, wp_buf) ||
        wp_buf[0] != 'E' || wp_buf[1] != 'F' || wp_buf[2] != 'I' ||
        wp_buf[3] != ' ' || wp_buf[4] != 'P' || wp_buf[5] != 'A' ||
        wp_buf[6] != 'R' || wp_buf[7] != 'T') return;
    uint32_t hsize = le32(wp_buf + 12), hcrc = le32(wp_buf + 16);
    uint64_t usable_first = wp_le64(wp_buf + 40), usable_last = wp_le64(wp_buf + 48);
    uint64_t table = wp_le64(wp_buf + 72);
    uint32_t entries = le32(wp_buf + 80), entrysize = le32(wp_buf + 84);
    uint32_t table_crc = le32(wp_buf + 88);
    if (hsize < 92 || hsize > 512 || wp_le64(wp_buf + 24) != 1 ||
        wp_le64(wp_buf + 32) != card_sectors - 1 || !entries || entries > 256 ||
        entrysize != 128 || table < 2 || usable_first >= usable_last ||
        usable_last >= card_sectors - 1 || table >= usable_first ||
        ((uint64_t)entries + 3) / 4 > usable_first - table) return;
    for (unsigned i = 16; i < 20; i++) wp_buf[i] = 0;
    if (crc32(wp_buf, hsize) != hcrc) { puts_uart("GPT header CRC mismatch\n"); return; }
    uint32_t crc = ~0U, found = 0;
    for (unsigned i = 0; i < (entries + 3) / 4; i++) {
        if (read_block(17, (uint32_t)table + i, wp_buf)) return;
        unsigned count = entries - i * 4; if (count > 4) count = 4;
        crc = wp_crc_update(crc, wp_buf, count * 128);
        for (unsigned j = 0; j < count; j++) {
            const uint8_t *p = wp_buf + j * 128;
            unsigned idx = i * 4 + j; uint8_t used = 0;
            for (unsigned k = 0; k < 16; k++) used |= p[k];
            if (!used) continue;
            wp_first[idx] = wp_le64(p + 32); wp_last[idx] = wp_le64(p + 40);
            if (wp_first[idx] < usable_first || wp_last[idx] > usable_last ||
                wp_first[idx] > wp_last[idx]) return;
            if (wp_userdata_name(p)) {
                if (found) { puts_uart("Duplicate userdata entry\n"); return; }
                found = idx + 1;
            }
        }
    }
    if (~crc != table_crc || !found) { puts_uart("GPT entries invalid\n"); return; }
    unsigned u = found - 1;
    for (unsigned i = 0; i < entries; i++)
        if (i != u && wp_first[i] && wp_first[i] <= wp_last[u] && wp_first[u] <= wp_last[i]) {
            puts_uart("userdata overlaps another partition\n"); return;
        }
    if (wp_last[u] - wp_first[u] < 4096 || wp_first[u] > 0xffffffffU ||
        wp_last[u] > 0xffffffffU) return;
    if (read_block(17, (uint32_t)wp_first[u] + 2, wp_buf)) return;
    if (wp_buf[0] != 'H' || (wp_buf[1] != 'X' && wp_buf[1] != '+')) {
        puts_uart("userdata is not an HFS+ volume; refusing write\n"); return;
    }
    uint32_t size = wp_be32(wp_buf + 40), blocks = wp_be32(wp_buf + 44);
    uint64_t volume_sectors = ((uint64_t)size * blocks + 511) / 512;
    uint64_t scratch = wp_last[u] - 2048;
    if (size < 512 || (size & (size - 1)) || !blocks ||
        volume_sectors > wp_last[u] - wp_first[u] + 1 ||
        scratch < wp_first[u] + volume_sectors + 2048) {
        puts_uart("No verified space outside HFS; refusing write\n"); return;
    }
    hex("userdata first LBA", wp_first[u]); hex("userdata last LBA", wp_last[u]);
    hex("HFS size in sectors", volume_sectors); hex("scratch LBA", scratch);
    if (read_block(17, (uint32_t)scratch, wp_original)) return;
    for (unsigned i = 0; i < 512; i++) wp_pattern[i] = wp_original[i] ^ (uint8_t)(0xa5U + i);
    hex("Original sector CRC32", crc32(wp_original, 512));
    puts_uart("Writing one scratch sector; original will be restored\n");
    int sent = 0, write_result = wp_write_one((uint32_t)scratch, wp_pattern, &sent);
    int pattern_ok = write_result == 0 && !read_block(17, (uint32_t)scratch, wp_buf);
    if (pattern_ok) for (unsigned i = 0; i < 512; i++)
        if (wp_buf[i] != wp_pattern[i]) { pattern_ok = 0; break; }
    if (!sent) { puts_uart("WRITE PROBE: command was not sent\n"); return; }
    puts_uart(pattern_ok ? "Scratch readback PASS; restoring\n" : "Scratch write/read failed; attempting restore\n");
    int restore_sent = 0;
    int restored = !wp_write_one((uint32_t)scratch, wp_original, &restore_sent) &&
        !read_block(17, (uint32_t)scratch, wp_buf);
    if (restored) for (unsigned i = 0; i < 512; i++)
        if (wp_buf[i] != wp_original[i]) { restored = 0; break; }
    if (!restored) { puts_uart("WRITE PROBE FAIL: scratch restore not verified\n"); return; }
    puts_uart(pattern_ok ? "WRITE PROBE PASS: original sector restored and verified\n" :
        "WRITE PROBE FAIL: original sector restored, but test write failed\n");
}
