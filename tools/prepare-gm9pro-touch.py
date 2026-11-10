#!/usr/bin/env python3
"""Connect the hardware-qualified GM9 sec_ts transport to iOS HID."""
from pathlib import Path
import sys

source = Path(sys.argv[1])
repo = Path(__file__).resolve().parent.parent

prefix = r'''#include <sys/param.h>
#include <sys/systm.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void IOSleep(unsigned int);
extern void thread_terminate_self(void);
extern void sweet_hid_touch(int, int, int, int);
void gm9pro_touch_start(void);
struct gm9_map { uintptr_t pa, va; unsigned size; };
static struct gm9_map gm9_maps[] = {
    {0x00117000,0,0x1000}, {0x00119000,0,0x1000},
    {0x00152000,0,0x1000}, {0x00171000,0,0x1000},
    {0x05065000,0,0x2000}, {0x0c175000,0,0x1000},
    {0x03102000,0,0x2000}, {0x03942000,0,0x2000},
    {0x00778000,0,0x7000}, {0x17911000,0,0x1000},
};
static uintptr_t gm9_va(uintptr_t pa)
{
    for (unsigned i = 0; i < sizeof(gm9_maps)/sizeof(gm9_maps[0]); i++)
        if (pa >= gm9_maps[i].pa && pa - gm9_maps[i].pa < gm9_maps[i].size)
            return gm9_maps[i].va + pa - gm9_maps[i].pa;
    panic("GM9 touch: unmapped register %llx", (unsigned long long)pa);
}
static uint32_t read32(uintptr_t pa) { return *(volatile uint32_t *)gm9_va(pa); }
static void write32(uintptr_t pa, uint32_t v) { *(volatile uint32_t *)gm9_va(pa) = v; }
static uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, cntpct_el0":"=r"(v)); return v; }
static uint64_t freq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0":"=r"(v)); return v; }
static void puts_uart(const char *s) { printf("%s", s); }
static void hex(const char *s, uint64_t v) { printf("%s=0x%llx\n", s, (unsigned long long)v); }
'''

rpm = (repo / 'shim/gm9pro-touch-rpm.inc').read_text()
rpm = rpm.replace('uintptr_t ram = ml_io_map', 'uintptr_t ram = ml_io_map')
rpm = rpm.replace('uintptr_t ram = RPM_MSGRAM_PA;',
                  'uintptr_t ram = gm9_va(RPM_MSGRAM_PA);')
rpm = rpm.replace('rpm.ipc = RPM_IPC_PA;', 'rpm.ipc = gm9_va(RPM_IPC_PA);')
assert 'uintptr_t ram = gm9_va(RPM_MSGRAM_PA);' in rpm
assert 'rpm.ipc = gm9_va(RPM_IPC_PA);' in rpm
# Waits >=1 ms yield the CPU; short bus settle delays remain bounded spins.
rpm = rpm.replace('    uint64_t end = ticks() + (freq() * us + 999999) / 1000000;',
                  '    if (us >= 1000) { IOSleep((us + 999) / 1000); return; }\n'
                  '    uint64_t end = ticks() + (freq() * us + 999999) / 1000000;')

s = (repo / 'shim/gm9pro-touch-gpu-probe.inc').read_text()
s = s.replace('#ifndef GM9_TOUCH_GPU_MODEL\n#include "gm9pro-touch-rpm.inc"\n#endif', rpm)
events = (repo / 'shim/gm9pro-sec-ts-events.inc').read_text()
events = events[:events.index('static void tg_events(void)')]
s = s.replace('#include "gm9pro-sec-ts-events.inc"', events +
              (repo / 'xnu/gm9pro-touch-runtime.inc').read_text())
start = s.index('static void tg_gpu(void)')
end = s.index('static void touch_gpu_probe(void)', start)
s = s[:start] + s[end:]
s = s.replace('static void touch_gpu_probe(void)', 'static void gm9_touch_hardware(void)')
s = s.replace('    tg_gpu();', '''    /* Configuration-only GPU evidence; leave collapsed engines untouched. */
    uint32_t gpu_gate = read32(GM9_GCC + 0x71004);
    printf("GM9 GPU: cfg AHB=%x\\n", gpu_gate);
    if ((gpu_gate & 1) && !(gpu_gate & 0x80000000U))
        printf("GM9 GPU: CX=%x GX=%x GFXCLK=%x; acceleration not enabled\\n",
            read32(GM9_GPUCC + 0x1004), read32(GM9_GPUCC + 0x1094),
            read32(GM9_GPUCC + 0x1098));''')
s = s.replace('GM9 TOUCH/GPU PROBE 3: RPM touch rails and reset; no storage commands',
              'GM9 HID: initialize sec_ts using qualified RPM/QUP path')
s = s.replace('    tg_events();', '    gm9_touch_events();')
s = s.replace('TOUCH/GPU PROBE COMPLETE; RPM votes remain until reset; reset manually',
              'GM9 HID: initialization or polling stopped; hardware pins restored')

suffix = r'''
static void gm9_touch_thread(__unused void *arg, __unused wait_result_t result)
{
    for (unsigned i = 0; i < sizeof(gm9_maps)/sizeof(gm9_maps[0]); i++) {
        gm9_maps[i].va = ml_io_map(gm9_maps[i].pa, gm9_maps[i].size);
        if (!gm9_maps[i].va) {
            printf("GM9 HID: MMIO mapping failed; input disabled\n");
            thread_terminate_self();
            return;
        }
    }
    gm9_touch_hardware();
    thread_terminate_self();
}
void gm9pro_touch_start(void)
{
    static unsigned started;
    int enabled = 1;
    PE_parse_boot_argn("gm9touch", &enabled, sizeof(enabled));
    if (!enabled || __atomic_exchange_n(&started, 1, __ATOMIC_RELAXED)) return;
    thread_t thread;
    kern_return_t kr = kernel_thread_start(gm9_touch_thread, NULL, &thread);
    if (kr == KERN_SUCCESS) thread_deallocate(thread);
    else printf("GM9 HID: thread start failed %d\n", kr);
}
'''
(source / 'bsd/dev/gm9pro_touch.c').write_text(prefix + s + suffix)
p = source / 'bsd/conf/files'
p.write_text(p.read_text() + '\nbsd/dev/gm9pro_touch.c\t\tstandard\n')

p = source / 'iokit/Kernel/arm/hid/SweetHID.cpp'
s = p.read_text()
changes = {
    'static SweetHIDTouch *gSweetTouch;':
        'static SweetHIDTouch *gSweetTouch;\nextern "C" void gm9pro_touch_start(void);',
    '\tIOLog("SWEET HID: touch registered\\n");':
        '\tIOLog("GM9 HID: touch service registered\\n");\n\tgm9pro_touch_start();',
    '(x > 1079 ? 1079 : x)': '(x > 1124 ? 1124 : x)',
    '(y > 2399 ? 2399 : y)': '(y > 2435 ? 2435 : y)',
    '((int64_t)x << 16) / 1080': '((int64_t)x << 16) / 1125',
    '((int64_t)y << 16) / 2400': '((int64_t)y << 16) / 2436',
}
for old, new in changes.items():
    assert s.count(old) == 1, old
    s = s.replace(old, new)
# Limit press/release logging on a persistent input device.
s = s.replace('if (was != down) {\n\t\tIOLog("SWEET HID: touch',
              'static unsigned gm9_log_count;\n\tif (was != down && gm9_log_count++ < 32) {\n\t\tIOLog("GM9 HID: touch')
p.write_text(s)
print('GM9 sec_ts transport, multitouch and cropped-viewport HID staged')
