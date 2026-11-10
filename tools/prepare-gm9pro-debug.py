#!/usr/bin/env python3
"""Add opt-in boot-wait snapshots without copying under scheduler locks."""
from pathlib import Path
import sys

source = Path(sys.argv[1])
repo = Path(__file__).resolve().parent.parent
p = source / 'osfmk/kern/exception.c'
p.write_bytes(p.read_bytes() + b'\n' + (repo / 'xnu/gm9pro-sample-task.inc').read_bytes())
(source / 'bsd/dev/gm9pro_sample.c').write_bytes((repo / 'xnu/gm9pro-sample.c').read_bytes())
p = source / 'bsd/conf/files'
p.write_bytes(p.read_bytes() + b'\nbsd/dev/gm9pro_sample.c\t\tstandard\n')
p = source / 'bsd/dev/gm9pro_emmc.c'
s = p.read_text(encoding='utf-8')
anchor = '    *dev = makedev(gm9_bmajor, found);'
assert s.count(anchor) == 1
s = 'extern void gm9_sample_start(void);\n' + s.replace(anchor, anchor + '\n    gm9_sample_start();')
p.write_bytes(s.encode())

p = source / 'iokit/Kernel/arm/SweetFramebuffer.cpp'
s = p.read_text(encoding='utf-8')
def replace(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)

replace('OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);',
'''OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);
static uint64_t gm9_debug_ticks, gm9_debug_swaps, gm9_debug_copies;
static uint64_t gm9_debug_notify, gm9_debug_delivered, gm9_debug_errors;
static uint32_t gm9_debug_last_notify;
extern "C" void gm9_framebuffer_sample(void);
extern "C" void gm9_framebuffer_sample(void)
{
    /* Software counters only: no display locks or MMIO accesses here. */
    printf("GM9 FRAME ticks=%llu swaps=%llu copies=%llu notify=%llu delivered=%llu errors=%llu last=0x%x\\n",
        __atomic_load_n(&gm9_debug_ticks, __ATOMIC_RELAXED),
        __atomic_load_n(&gm9_debug_swaps, __ATOMIC_RELAXED),
        __atomic_load_n(&gm9_debug_copies, __ATOMIC_RELAXED),
        __atomic_load_n(&gm9_debug_notify, __ATOMIC_RELAXED),
        __atomic_load_n(&gm9_debug_delivered, __ATOMIC_RELAXED),
        __atomic_load_n(&gm9_debug_errors, __ATOMIC_RELAXED),
        __atomic_load_n(&gm9_debug_last_notify, __ATOMIC_RELAXED));
}''')
replace('\tuint64_t count = ++fb->fVsyncCount;',
        '\tuint64_t count = ++fb->fVsyncCount;\n\t__atomic_fetch_add(&gm9_debug_ticks, 1, __ATOMIC_RELAXED);')
replace('AppleCLCD::swap(const uint8_t *rec)\n{',
        'AppleCLCD::swap(const uint8_t *rec)\n{\n\t__atomic_fetch_add(&gm9_debug_swaps, 1, __ATOMIC_RELAXED);')
replace('\t\tgm9pro_display_end_frame();',
        '\t\tgm9pro_display_end_frame();\n\t\t__atomic_fetch_add(&gm9_debug_copies, 1, __ATOMIC_RELAXED);')
replace('\tsendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,\n\t    kIOUserNotifyOptionCanDrop);',
'''\t__atomic_fetch_add(&gm9_debug_notify, 1, __ATOMIC_RELAXED);
\tIOReturn result = sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
\t    kIOUserNotifyOptionCanDrop);
\t__atomic_store_n(&gm9_debug_last_notify, (uint32_t)result, __ATOMIC_RELAXED);
\tif (result == kIOReturnSuccess)
\t\t__atomic_fetch_add(&gm9_debug_delivered, 1, __ATOMIC_RELAXED);
\telse
\t\t__atomic_fetch_add(&gm9_debug_errors, 1, __ATOMIC_RELAXED);''')
p.write_bytes(s.encode())
print('GM9 bounded boot-wait sampler staged (gm9sample=1)')
