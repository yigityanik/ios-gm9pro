#!/usr/bin/env python3
"""Exercise the standalone QUP probe against a bounded MMIO model on Linux."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
code = (repo / 'shim/gm9pro-touch-gpu-probe.inc').read_text()
code = code.replace('__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
model = r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define GM9_TOUCH_GPU_MODEL 1
static int tg_rpm_ldo(uint32_t resource, unsigned id, uint32_t uv) {
    assert((resource == 0x616f646c && id == 11 && uv == 1800000) ||
           (resource == 0x626f646c && id == 6 && uv == 3300000));
    return 1;
}
static void tg_delay_us(unsigned us) { (void)us; }
static uint64_t now;
static unsigned mode, fifo, writes, rxpos, reset_count;
static uint32_t state = 0x14, tx[3], rootcfg = 0x12340000;
static uint64_t ticks(void) { return ++now; }
static uint64_t freq(void) { return 10000; }
static void puts_uart(const char *s) { (void)s; }
static void hex(const char *s, uint64_t v) { (void)s; (void)v; }
static uint32_t read32(uintptr_t a) {
    assert(a < 0x05000000 || a >= 0x05065000); /* no GPU core window */
    if (a == 0x0c175004) return state;
    if (a == 0x00119024) return rootcfg;
    if (a == 0x0c175404) return mode == 1 ? 8 : 0;
    if (a == 0x0c175018)
        return mode == 2 ? 0 : 0xc00 | (rxpos < 12 ? 0x20 : 0);
    if (a == 0x0c175218) {
        uint8_t data[12] = {0x87, 3, 0x20, 0x66, 0x01, 0, 0, 0};
        unsigned off = rxpos; rxpos += 4;
        return data[off] | data[off+1]<<8 | data[off+2]<<16 | data[off+3]<<24;
    }
    return 0;
}
static void write32(uintptr_t a, uint32_t v) {
    /* Storage, SMMU and GPU engine commands must never be issued. */
    assert(!(a >= 0x0c0c4000 && a < 0x0c0c5000));
    assert(!(a >= 0x05000000 && a < 0x05065000));
    writes++;
    if (a == 0x0c17500c) {
        state = 0x14; fifo = rxpos = 0; reset_count++;
    }
    if (a == 0x0c175004) state = v | 0x14;
    if (a == 0x0c175110) { assert(fifo < 3); tx[fifo++] = v; }
    if (a == 0x00119024) rootcfg = v;
}
'''
checks = r'''
int main(void) {
    uint8_t out[8] = {0};
    assert(tg_read(0x22, out, 3) == 0);
    assert(out[0] == 0x20 && out[1] == 0x66 && out[2] == 1);
    assert(tx[0] == 0x01829081 && tx[1] == 0x87918122 && tx[2] == 3);
    assert(reset_count == 2);
    assert(tg_command(0x10, 0, 0) == 0);
    assert(tx[0] == 0x01839081 && tx[1] == 0x10);
    assert(tg_command(0x30, 0x61, 2) == 0);
    assert(tx[0] == 0x03839081 && tx[1] == 0x6130);
    unsigned before = writes;
    assert(tg_command(0xd7, 0, 0) == -1 && writes == before);
    assert(tg_command(0x30, 0, 1) == -1 && writes == before);
    assert(tg_read(0x42, out, 3) == -1 && writes == before);
    assert(tg_read(0x22, out, 0) == -1 && writes == before);
    assert(tg_read(0x22, out, 9) == -1 && writes == before);
    mode = 1; out[0] = 0xa5;
    assert(tg_read(0x22, out, 3) == -1 && out[0] == 0xa5);
    mode = 2; uint64_t start = now;
    assert(tg_read(0x22, out, 3) == -1 && now - start < 3000);
    mode = 1;
    touch_gpu_probe();
    assert(rootcfg == 0x12340000);
    struct gm9_touch_point point;
    uint8_t packet[8] = {0x44, 0xab, 0xcd, 0xef, 7, 4, 0x3f, 2};
    assert(tg_decode(packet, &point));
    assert(point.id == 0 && point.action == 1 && point.x == 0xabe && point.y == 0xcdf);
    assert(point.pressure == 63 && point.type == 0 && point.remaining == 2);
    packet[0] = 0xa8; /* tenth finger, move */
    assert(tg_decode(packet, &point) && point.id == 9 && point.action == 2);
    packet[0] = 0xc4;
    assert(tg_decode(packet, &point) && point.action == 3);
    packet[0] = 0; assert(!tg_decode(packet, &point));
    packet[0] = 0x45; assert(!tg_decode(packet, &point));
    packet[0] = 0x7c; assert(!tg_decode(packet, &point));
    puts("PASS: QUP tags/RX, invalid requests, NACK, timeout, clock restoration, no storage/GPU engine writes");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'probe.c'
    binary = Path(directory) / 'probe-test'
    source.write_text(model + code + checks)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(repo / 'shim'), '-fsanitize=undefined,address', str(source), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)

rpm_code = (repo / 'shim/gm9pro-touch-rpm.inc').read_text()
rpm_code = rpm_code.replace('__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
rpm_code = rpm_code.replace('__asm__ volatile("yield");', '(void)0;')
rpm_model = r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
static uint64_t now;
static uint64_t ticks(void) { return ++now; }
static uint64_t freq(void) { return 10000; }
static void hex(const char *s, uint64_t v) { (void)s; (void)v; }
'''
rpm_checks = r'''
static uint32_t txbuf[34], rxbuf[34], ipc[4];
static void setup(unsigned channel, unsigned message, unsigned length) {
    memset(&rpm, 0, sizeof(rpm));
    memset(txbuf, 0, sizeof(txbuf)); memset(rxbuf, 0, sizeof(rxbuf));
    rpm.up = rpm.remote_open = rpm.open_acked = rpm.mapped = true;
    rpm.rcid = 7; rpm.ipc = (uintptr_t)ipc;
    rpm.tx = (struct rpm_fifo){txbuf, txbuf+1, txbuf+2, 128};
    rpm.rx = (struct rpm_fifo){rxbuf, rxbuf+1, rxbuf+2, 128};
    rxbuf[1] = 40;
    uint32_t packet[] = {9 | (channel<<16), 0, 20, 0,
        0x716572, 12, 0x2367736d, length, message, 0};
    memcpy(rxbuf+2, packet, sizeof(packet));
}
int main(void) {
    setup(7, 1, 4);
    assert(tg_rpm_ldo(0x616f646c, 11, 1800000));
    assert(txbuf[2+8] == 0x616f646c && txbuf[2+9] == 11);
    assert(txbuf[2+16] == 1800000 && ipc[2] == 1);
    setup(7, 1, 4);
    assert(tg_rpm_ldo(0x626f646c, 6, 3300000));
    assert(txbuf[2+8] == 0x626f646c && txbuf[2+16] == 3300000);
    setup(7, 99, 4); assert(!tg_rpm_ldo(0x616f646c, 11, 1800000));
    setup(7, 1, 100); assert(!tg_rpm_ldo(0x616f646c, 11, 1800000));
    setup(8, 1, 4); assert(!tg_rpm_ldo(0x616f646c, 11, 1800000));
    setup(7, 1, 4); txbuf[1] = 129;
    assert(!tg_rpm_ldo(0x616f646c, 11, 1800000));
    puts("PASS: RPM rail packets, ACK ID/channel checks, malformed length and FIFO bounds");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'rpm.c'
    binary = Path(directory) / 'rpm-test'
    source.write_text(rpm_model + rpm_code + rpm_checks)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=undefined,address', str(source), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)
