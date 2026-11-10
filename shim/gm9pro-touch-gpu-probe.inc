/* SDM660 prerequisites, from the GM9 Pro DTS and GCC/pinctrl drivers.
 * Standalone diagnostic: no storage, DMA, firmware upload or GPU commands.
 * QUP v2 tag/PIO interface: Linux drivers/i2c/busses/i2c-qup.c.
 */
#define GM9_GCC 0x00100000UL
#define GM9_QUP 0x0c175000UL
#define GM9_GPUCC 0x05065000UL
#define GM9_GPIO2 0x03102000UL
#define GM9_GPIO3 0x03103000UL
#define GM9_GPIO66 0x03942000UL
#define GM9_GPIO67 0x03943000UL
#ifndef GM9_TOUCH_GPU_MODEL
#include "gm9pro-touch-rpm.inc"
#endif

static int tg_wait(uintptr_t addr, uint32_t mask, uint32_t value)
{
    uint64_t end = ticks() + freq() / 10;
    do {
        if ((read32(addr) & mask) == value) return 0;
    } while (ticks() < end);
    hex("timeout address", addr);
    hex("timeout value", read32(addr));
    return -1;
}

static void tg_write(uintptr_t addr, uint32_t value)
{
    write32(addr, value);
    __asm__ volatile("dsb sy" ::: "memory");
}

static int tg_state(unsigned state)
{
    if (tg_wait(GM9_QUP + 4, 4, 4)) return -1;
    tg_write(GM9_QUP + 4, state);
    return tg_wait(GM9_QUP + 4, 7, state | 4);
}

static unsigned tg_quiet;
static int tg_transfer(const uint8_t *tx, unsigned tx_len,
                       uint8_t *out, unsigned len)
{
    uint8_t rx[16];
    unsigned expected = len ? len + 2 : 0;
    if (!tx_len || tx_len > 9 || len > 8 || (len && !out)) return -1;
    tg_write(GM9_QUP + 0x0c, 1);
    if (tg_wait(GM9_QUP + 4, 7, 4)) return -1;
    tg_write(GM9_QUP, len ? 0x207 : 0x287); /* I2C mini-core, 8-bit words */
    tg_write(GM9_QUP + 0x408, 1); /* v2 byte tags */
    if (tg_wait(GM9_QUP + 4, 0x10, 0x10)) return -1;
    tg_write(GM9_QUP + 8, 0xc000); /* packed/unpacked FIFO, no DMA */
    tg_write(GM9_QUP + 0x100, 0);
    tg_write(GM9_QUP + 0x200, 0);
    tg_write(GM9_QUP + 0x150, tx_len);
    tg_write(GM9_QUP + 0x208, expected);
    tg_write(GM9_QUP + 0x028, 0); /* status visible to polling */
    if (tg_state(1)) return -1;
    tg_write(GM9_QUP + 0x400, 0x35d); /* 19.2 MHz / 100 kHz */
    if (tg_state(3)) return -1;
    for (unsigned i = 0; i < tx_len; i += 4) {
        uint32_t word = 0;
        for (unsigned j = 0; j < 4 && i + j < tx_len; j++)
            word |= (uint32_t)tx[i + j] << (8 * j);
        tg_write(GM9_QUP + 0x110, word);
    }
    if (tg_state(1)) return -1;
    unsigned received = 0;
    uint64_t end = ticks() + freq() / 5;
    int rc = -1;
    do {
        uint32_t op = read32(GM9_QUP + 0x18);
        uint32_t bus = read32(GM9_QUP + 0x404);
        uint32_t err = read32(GM9_QUP + 0x1c);
        if ((bus & 0x038000fc) || (err & 0x7c)) {
            hex("QUP I2C error", bus); hex("QUP FIFO error", err);
            break;
        }
        if ((op & 0x20) && received < expected) {
            uint32_t word = read32(GM9_QUP + 0x218);
            for (unsigned j = 0; j < 4 && received < expected; j++)
                rx[received++] = (word >> (8 * j)) & 255;
        }
        if (op & 0x300) tg_write(GM9_QUP + 0x18, op & 0x300);
        if (received == expected && (op & (len ? 0x800 : 0x400)) && !(bus & 0x100)) {
            rc = 0;
            break;
        }
    } while (ticks() < end);
    if (!rc) {
        if (len && !tg_quiet) hex("QUP RX tags", rx[0] | ((unsigned)rx[1] << 8));
        for (unsigned i = 0; i < len; i++) out[i] = rx[i + 2];
    } else {
        hex("QUP final operational", read32(GM9_QUP + 0x18));
        hex("QUP final I2C status", read32(GM9_QUP + 0x404));
        hex("QUP received bytes", received);
    }
    /* Abort a failed bus transaction; never retry indefinitely. */
    tg_write(GM9_QUP + 0x0c, 1);
    if (tg_wait(GM9_QUP + 4, 7, 4)) return -1;
    return rc;
}

/* Only documented identification/event registers and volatile sense controls.
 * There is no interface for firmware download, calibration, erase or NVM.
 */
static int tg_read(unsigned reg, uint8_t *out, unsigned len)
{
    if (!len || len > 8 || (reg != 0x22 && reg != 0x55 && reg != 0x60 &&
                            reg != 0x30 && reg != 0x21)) return -1;
    uint8_t tx[9] = {0x81, 0x90, 0x82, 1, reg, 0x81, 0x91, 0x87, len};
    return tg_transfer(tx, sizeof(tx), out, len);
}
static int tg_command(unsigned reg, unsigned value, unsigned len)
{
    if (!((reg == 0x30 && len == 2) ||
          ((reg == 0x10 || reg == 0x11 || reg == 0x62) && len == 0))) return -1;
    uint8_t tx[7] = {0x81, 0x90, 0x83, len + 1, reg, value & 255, value >> 8};
    return tg_transfer(tx, 5 + len, 0, 0);
}
#include "gm9pro-sec-ts-events.inc"

static void tg_gpu(void)
{
    uintptr_t gate = GM9_GCC + 0x71004;
    uint32_t saved = read32(gate);
    puts_uart("GM9 GPU: configuration clock and power-domain inspection\n");
    hex("GCC GPU CFG AHB before", saved);
    tg_write(gate, saved | 1);
    if (tg_wait(gate, 0x80000000U, 0)) {
        puts_uart("GPU configuration clock unavailable; GPUCC not read\n");
    } else {
        hex("GPU CX GDSCR", read32(GM9_GPUCC + 0x1004));
        hex("GPU GX GDSCR", read32(GM9_GPUCC + 0x1094));
        hex("GPU CXO CBCR", read32(GM9_GPUCC + 0x1020));
        hex("GPU GFX3D CBCR", read32(GM9_GPUCC + 0x1098));
        hex("GPU RBBMTIMER CBCR", read32(GM9_GPUCC + 0x10d0));
        puts_uart("GPU core/SMMU access skipped until rails and clocks are qualified\n");
    }
    tg_write(gate, saved);
}

static void touch_gpu_probe(void)
{
    puts_uart("GM9 TOUCH/GPU PROBE 3: RPM touch rails and reset; no storage commands\n");
    tg_gpu();
    hex("GPIO2 config", read32(GM9_GPIO2));
    hex("GPIO3 config", read32(GM9_GPIO3));
    hex("touch RESET config", read32(GM9_GPIO66));
    hex("touch RESET level", read32(GM9_GPIO66 + 4));
    hex("touch IRQ config", read32(GM9_GPIO67));
    hex("touch IRQ level", read32(GM9_GPIO67 + 4));
    uint32_t vote = read32(GM9_GCC + 0x52004);
    uint32_t gate = read32(GM9_GCC + 0x19008);
    uint32_t cfg = read32(GM9_GCC + 0x19024);
    uint32_t command = read32(GM9_GCC + 0x19020);
    uint32_t pins[2] = {read32(GM9_GPIO2), read32(GM9_GPIO3)};
    uint32_t reset_cfg = read32(GM9_GPIO66);
    uint32_t reset_out = read32(GM9_GPIO66 + 4) & 2;
    uint32_t irq_cfg = read32(GM9_GPIO67);
    hex("QUP1 root command", command);
    hex("QUP1 root config", cfg);
    tg_write(GM9_GCC + 0x52004, vote | (1U << 17));
    if (tg_wait(GM9_GCC + 0x17004, 0x80000000U, 0)) goto restore;
    /* XO / 1. Preserve root enable controls, select only source and divider. */
    tg_write(GM9_GCC + 0x19024, cfg & ~0x71fU);
    tg_write(GM9_GCC + 0x19020, command | 1);
    if (tg_wait(GM9_GCC + 0x19020, 1, 0)) goto restore;
    tg_write(GM9_GCC + 0x19008, gate | 1);
    if (tg_wait(GM9_GCC + 0x19008, 0x80000000U, 0)) goto restore;
    /* GPIO2/3 function 3 = BLSP I2C1; UART TX/RX0/1 left untouched.
     * Pulls/drive follow active DTS: no pull, 2 mA, peripheral controls OE.
     */
    tg_write(GM9_GPIO2, (pins[0] & ~0x3ffU) | 12);
    tg_write(GM9_GPIO3, (pins[1] & ~0x3ffU) | 12);
    hex("QUP hardware version", read32(GM9_QUP + 0x30));
    hex("QUP initial state", read32(GM9_QUP + 4));
    /* Hold reset during power-up. This GPIO is not a PMIC voltage selector. */
    tg_write(GM9_GPIO66 + 4, 0);
    tg_write(GM9_GPIO66, 0x3c3); /* GPIO, 16 mA, pull-up, output */
    tg_write(GM9_GPIO67, 0x1c3); /* GPIO, 16 mA, pull-up, input */
    puts_uart("RPM: opening SDM660 transport at 0x778000\n");
    if (!tg_rpm_ldo(0x616f646cU, 11, 1800000)) {
        puts_uart("RPM PM660 L11 enable failed; reset held, ID read cancelled\n");
        goto restore;
    }
    puts_uart("RPM PM660 L11 1800000 uV ACK\n");
    tg_delay_us(1000);
    if (!tg_rpm_ldo(0x626f646cU, 6, 3300000)) {
        puts_uart("RPM PM660L L6 enable failed; reset held, ID read cancelled\n");
        goto restore;
    }
    puts_uart("RPM PM660L L6 3300000 uV ACK\n");
    tg_delay_us(10000);
    tg_write(GM9_GPIO66 + 4, 2);
    tg_delay_us(70000);
    hex("touch RESET after power", read32(GM9_GPIO66 + 4));
    hex("touch IRQ after power", read32(GM9_GPIO67 + 4));
    uint8_t data[8];
    if (tg_read(0x22, data, 5)) {
        puts_uart("SEC_TS ID read failed; check rails/reset/bus before HID integration\n");
        goto restore;
    }
    hex("SEC_TS DEVICE_ID", data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16));
    hex("SEC_TS DEVICE_ID suffix", data[3] | ((uint32_t)data[4] << 8));
    if ((!data[0] && !data[1] && !data[2]) ||
        (data[0] == 255 && data[1] == 255 && data[2] == 255)) {
        puts_uart("SEC_TS invalid identity; event reads cancelled\n");
        goto restore;
    }
    if (tg_read(0x55, data, 1)) goto restore;
    hex("SEC_TS BOOT_STATUS", data[0]);
    puts_uart("SEC_TS ID PASS; controller access confirmed, not yet an iOS HID device\n");
    if (data[0] != 0x20) {
        puts_uart("SEC_TS not in application mode; sense commands cancelled\n");
        goto restore;
    }
    tg_events();
restore:
    tg_write(GM9_GPIO66 + 4, reset_out);
    tg_write(GM9_GPIO66, reset_cfg);
    tg_write(GM9_GPIO67, irq_cfg);
    tg_write(GM9_GPIO2, pins[0]);
    tg_write(GM9_GPIO3, pins[1]);
    tg_write(GM9_GCC + 0x19008, gate);
    tg_write(GM9_GCC + 0x19024, cfg);
    tg_write(GM9_GCC + 0x19020, command | 1);
    (void)tg_wait(GM9_GCC + 0x19020, 1, 0);
    tg_write(GM9_GCC + 0x52004, vote);
    puts_uart("TOUCH/GPU PROBE COMPLETE; RPM votes remain until reset; reset manually\n");
}
