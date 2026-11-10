/* Samsung sec_ts 8-byte coordinate packet, same layout as the stock driver.
 * The diagnostic reports raw coordinates; panel orientation is not assumed.
 */
struct gm9_touch_point {
    unsigned id, action, x, y, pressure, type, remaining;
};
static int tg_decode(const uint8_t *p, struct gm9_touch_point *v)
{
    if ((p[0] & 3) != 0) return 0;
    unsigned id = (p[0] >> 2) & 15, action = p[0] >> 6;
    if (id < 1 || id > 10 || action < 1 || action > 3) return 0;
    *v = (struct gm9_touch_point){
        id - 1, action, ((unsigned)p[1] << 4) | (p[3] >> 4),
        ((unsigned)p[2] << 4) | (p[3] & 15), p[6] & 63,
        ((p[6] >> 6) << 2) | (p[7] >> 6), p[7] & 63
    };
    return 1;
}
static void tg_events(void)
{
    uint8_t p[8];
    if (tg_read(0x30, p, 2)) return;
    unsigned functions = p[0] | ((unsigned)p[1] << 8);
    hex("SEC_TS initial touch functions", functions);
    /* Preserve vendor bits, enable stock TOUCH | PALM | WET function mask. */
    if (tg_command(0x30, functions | 0x61, 2) ||
        tg_command(0x62, 0, 0) || tg_command(0x10, 0, 0)) {
        puts_uart("SEC_TS sense initialization failed\n");
        return;
    }
    puts_uart("SEC_TS SENSE ON: touch and swipe for 60 seconds\n");
    unsigned events = 0, coordinates = 0, logged = 0;
    unsigned failed = 0;
    uint64_t until = ticks() + 60 * freq(), report = ticks() + 5 * freq();
    tg_quiet = 1;
    while (ticks() < until) {
        /* Active-low interrupt; polling avoids changing GIC/TLMM routing.
         * Limit each drain so a stuck line cannot starve the timeout.
         */
        for (unsigned i = 0; i < 32 && !(read32(GM9_GPIO67 + 4) & 1); i++) {
            if (tg_read(0x60, p, 8)) { failed = 1; break; }
            if (!p[0]) break; /* empty FIFO, not a finger release */
            events++;
            struct gm9_touch_point point;
            if (tg_decode(p, &point)) {
                coordinates++;
                if (logged++ < 128) {
                    hex("SEC_TS contact/action", (point.id << 8) | point.action);
                    hex("SEC_TS raw X/Y", ((uint64_t)point.x << 32) | point.y);
                    hex("SEC_TS pressure/type", (point.type << 8) | point.pressure);
                }
            } else if (events < 12) {
                hex("SEC_TS status bytes0..3", (unsigned)p[0] |
                    ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
                    ((unsigned)p[3] << 24));
            }
        }
        if (failed) break;
        if (ticks() >= report) {
            hex("SEC_TS event count", events);
            hex("SEC_TS coordinate count", coordinates);
            hex("SEC_TS live IRQ", read32(GM9_GPIO67 + 4));
            report = ticks() + 5 * freq();
        }
        tg_delay_us(5000);
    }
    tg_quiet = 0;
    if (!failed) (void)tg_command(0x11, 0, 0);
    hex("SEC_TS total coordinate events", coordinates);
    puts_uart(failed ? "SEC_TS event bus error; polling stopped\n" :
        "SEC_TS event test finished; not connected to iOS HID yet\n");
}
