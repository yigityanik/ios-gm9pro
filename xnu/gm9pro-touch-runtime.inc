/* One polling thread owns QUP1 and the RPM transport. Hardware setup and
 * packet decoding are shared with the successful standalone sec_ts probe.
 */
static bool gm9_down[10];
static int gm9_x[10], gm9_y[10];
static unsigned gm9_input_count;

static void gm9_release_all(void)
{
    for (unsigned i = 0; i < 10; i++) {
        if (gm9_down[i]) sweet_hid_touch(i, 0, gm9_x[i], gm9_y[i]);
        gm9_down[i] = false;
    }
}

static void gm9_touch_events(void)
{
    uint8_t p[8];
    if (tg_read(0x30, p, 2)) return;
    unsigned functions = p[0] | ((unsigned)p[1] << 8);
    if (tg_command(0x30, functions | 0x61, 2) ||
        tg_command(0x62, 0, 0) || tg_command(0x10, 0, 0)) {
        printf("GM9 HID: sense initialization failed\n");
        return;
    }
    printf("GM9 HID: sec_ts ready, raw 1080x2160 -> cropped 1125x2436 viewport\n");
    tg_quiet = 1;
    uint64_t report = ticks() + 60 * freq();
    for (;;) {
        for (unsigned i = 0; i < 64 && !(read32(GM9_GPIO67 + 4) & 1); i++) {
            if (tg_read(0x60, p, 8)) goto failed;
            if (!p[0]) break;
            struct gm9_touch_point point;
            if (!tg_decode(p, &point)) {
                /* Stock queue overflow and unexpected reset invalidate slots. */
                if ((p[0] & 3) == 1 &&
                    ((((p[0] >> 2) & 15) == 1 && p[1] == 1) ||
                     (((p[0] >> 2) & 15) == 2 && p[1] == 0))) {
                    gm9_release_all();
                    printf("GM9 HID: controller reset/overflow; input stopped\n");
                    goto failed;
                }
                continue;
            }
            unsigned id = point.id;
            bool down = point.action != 3 &&
                (point.type == 0 || point.type == 3 || point.type == 6);
            /* Invert the framebuffer's centered crop, not a 4095-based scale.
             * Reject out-of-panel packets rather than creating edge touches.
             */
            if (point.x >= 1080 || point.y >= 2160) {
                if (gm9_down[id]) sweet_hid_touch(id, 0, gm9_x[id], gm9_y[id]);
                gm9_down[id] = false;
                continue;
            }
            int x = point.x + 22, y = point.y + 138;
            if (down || gm9_down[id]) {
                sweet_hid_touch(id, down, x, y);
                gm9_input_count++;
            }
            gm9_down[id] = down;
            gm9_x[id] = x; gm9_y[id] = y;
        }
        if (ticks() >= report) {
            printf("GM9 HID: %u coordinate events submitted\n", gm9_input_count);
            report = ticks() + 60 * freq();
        }
        IOSleep(5);
    }
failed:
    tg_quiet = 0;
    gm9_release_all();
    printf("GM9 HID: bus/controller failure; released all contacts, polling stopped\n");
}
