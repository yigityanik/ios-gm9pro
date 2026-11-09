/* Validate the optional larger RAM window against ABL's live FDT. */
static uint32_t ram_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
        (uint32_t)p[2] << 8 | p[3];
}
static uint64_t ram_be64(const uint8_t *p)
{
    return (uint64_t)ram_be32(p) << 32 | ram_be32(p + 4);
}
static int ram_name(const uint8_t *p, unsigned len, const char *name)
{
    unsigned i = 0;
    while (name[i] && i < len && p[i] == (uint8_t)name[i]) i++;
    return !name[i] && i < len && !p[i];
}
static int ram_overlap(uint64_t a, uint64_t size, uint64_t start, uint64_t end)
{
    return size && (a + size < a || (a < end && a + size > start));
}
static int ram_window_valid(uintptr_t addr, uint64_t start, uint64_t end)
{
    const uint8_t *f = (const uint8_t *)addr;
    if (ram_be32(f) != 0xd00dfeed || start >= end) return 0;
    uint32_t total = ram_be32(f + 4), off = ram_be32(f + 8);
    uint32_t strings = ram_be32(f + 12), reserve = ram_be32(f + 16);
    uint32_t strings_len = ram_be32(f + 32), struct_len = ram_be32(f + 36);
    if (total < 40 || total > 0x200000 || off > total || struct_len > total - off ||
        strings > total || strings_len > total - strings || reserve > total) return 0;
    unsigned entries = 0;
    for (;;) {
        if (reserve > total || total - reserve < 16 || ++entries > 128) return 0;
        uint64_t a = ram_be64(f + reserve), size = ram_be64(f + reserve + 8);
        reserve += 16;
        if (!a && !size) break;
        if (ram_overlap(a, size, start, end)) return 0;
    }
    uint64_t banks_start[16], banks_end[16];
    unsigned banks = 0, depth = 0, memory_depth = 0, reserved_depth = 0;
    uint32_t limit = off + struct_len;
    int complete = 0;
    while (off <= limit && limit - off >= 4) {
        uint32_t token = ram_be32(f + off); off += 4;
        if (token == 1) {
            unsigned len = 0;
            while (len < limit - off && f[off + len]) len++;
            if (len == limit - off || ++depth > 64) return 0;
            if (depth == 2 && (ram_name(f + off, len + 1, "memory") ||
                (len >= 7 && f[off] == 'm' && f[off+1] == 'e' &&
                 f[off+2] == 'm' && f[off+3] == 'o' && f[off+4] == 'r' &&
                 f[off+5] == 'y' && f[off+6] == '@'))) memory_depth = depth;
            if (depth == 2 && ram_name(f + off, len + 1, "reserved-memory")) reserved_depth = depth;
            off = (off + len + 4) & ~3U;
        } else if (token == 2) {
            if (!depth) return 0;
            if (memory_depth == depth) memory_depth = 0;
            if (reserved_depth == depth) reserved_depth = 0;
            depth--;
        } else if (token == 3) {
            if (limit - off < 8) return 0;
            uint32_t len = ram_be32(f + off), name = ram_be32(f + off + 4); off += 8;
            if (len > limit - off || name >= strings_len) return 0;
            const uint8_t *key = f + strings + name, *v = f + off;
            unsigned left = strings_len - name;
            if ((depth == 1 || depth == reserved_depth) &&
                (ram_name(key, left, "#address-cells") || ram_name(key, left, "#size-cells")) &&
                (len != 4 || ram_be32(v) != 2)) return 0;
            if (ram_name(key, left, "reg") && (memory_depth == depth || reserved_depth)) {
                if (!len || len % 16) return 0;
                for (unsigned n = 0; n < len; n += 16) {
                    uint64_t a = ram_be64(v+n), size = ram_be64(v+n+8);
                    if (a + size < a) return 0;
                    if (reserved_depth && ram_overlap(a, size, start, end)) return 0;
                    if (memory_depth == depth && size) {
                        if (banks == 16) return 0;
                        banks_start[banks] = a; banks_end[banks++] = a + size;
                    }
                }
            }
            off = (off + len + 3) & ~3U;
        } else if (token == 9) {
            complete = depth == 0; break;
        } else if (token != 4) return 0;
    }
    if (!complete) return 0;
    uint64_t cursor = start;
    for (unsigned pass = 0; pass < banks; pass++) {
        uint64_t next = cursor;
        for (unsigned i = 0; i < banks; i++)
            if (banks_start[i] <= cursor && banks_end[i] > next) next = banks_end[i];
        if (next >= end) return 1;
        if (next == cursor) return 0;
        cursor = next;
    }
    return 0;
}
