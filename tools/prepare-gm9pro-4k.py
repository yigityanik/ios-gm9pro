#!/usr/bin/env python3
"""Opt-in GM9 Pro 4 KiB/SMP overrides, after the regular device preparation."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) > 2 else 'native-4k'
if mode not in ('native-4k', '4k-hw-16k-sw'):
    raise SystemExit(f'unknown GM9 page model: {mode}')
hybrid_pages = mode == '4k-hw-16k-sw'

def edit(path, old, new):
    p = source / path
    text = p.read_text(encoding='utf-8')
    if text.count(old) != 1:
        raise RuntimeError(f'{path}: expected one occurrence of {old!r}')
    p.write_bytes(text.replace(old, new).encode())

if hybrid_pages:
    edit('pexpert/pexpert/arm64/SWEET.h',
         '#define __ARM_V8_CRYPTO_EXTENSIONS__ 1\n'
         '/* no __ARM_PAN_AVAILABLE__: PAN is ARMv8.1, the Pixel 1 Kryo is v8.0 */\n'
         '#define __ARM_16K_PG__            1',
         '/* Common ARMv8-A A53/A73 baseline: no crypto, PAN or hardware 16 KiB granule. */\n'
         '#define __ARM64_PMAP_SUBPAGE_L1__ 1')
    # Keep the SDM660 Kryo 260 on the ARMv8.0 ISA baseline; -march alone
    # can leave Apple's arm64 target features enabled, including LSE atomics.
    edit('makedefs/MakeInc.def', '-mcpu=cortex-a73', '-mcpu=cortex-a53')
    p = source / 'osfmk/arm64/corecrypto/sha256_compress_arm64.s'
    s = p.read_text(encoding='utf-8')
    if not s.startswith('\t.arch_extension sha2\n'):
        p.write_text('\t.arch_extension sha2\n' + s, encoding='utf-8')
else:
    edit('pexpert/pexpert/arm64/SWEET.h',
         '#define __ARM_16K_PG__            1',
         '/* Native 4 KiB: leave __ARM_16K_PG__ undefined (proc_reg.h uses ifdef). */\n#define __ARM64_PMAP_SUBPAGE_L1__ 1\n#define XNU_NATIVE_CS_HASH_CACHE 1')
    edit('makedefs/MakeInc.def', '-mcpu=cortex-a73', '-mcpu=cortex-a53')
# The macOS build defaults to a -2 TiB link address and 16K large-memory VA
# layout. A 4K L1 root cannot translate that address: use the existing 4K layout.
edit('makedefs/MakeInc.def', 'LARGE_MEMORY_DEFINE=-DARM_LARGE_MEMORY=1',
     'LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY')
edit('makedefs/MakeInc.def', 'ARM64_PLKSEG_ADDR  =0xfffffe0004004000',
     'ARM64_PLKSEG_ADDR  =0xfffffff004004000')
edit('makedefs/MakeInc.def', 'ARM64_LINK_ADDR    =0xfffffe0007004000',
     'ARM64_LINK_ADDR    =0xfffffff007004000')


# A three-level 4K pmap uses the iOS user VA limit; the macOS 128 TiB
# limit cannot fit its L1 geometry (and XNU checks this at compile time).
edit('osfmk/mach/arm/vm_param.h',
     '#if defined(XNU_PLATFORM_MacOSX) || defined(XNU_PLATFORM_DriverKit)',
     '#if (defined(XNU_PLATFORM_MacOSX) || defined(XNU_PLATFORM_DriverKit)) && !defined(SWEET)')

# The macOS 128 GiB zone reservation cannot fit the 4K non-large VA map.
# Select the existing mobile policy, including its metadata sizing. zsize
# controls the physical limit only and cannot repair this VA reservation.
edit('osfmk/kern/zalloc.c',
     '#if XNU_PLATFORM_MacOSX\n#define ZONE_MAP_MAX',
     '#if XNU_PLATFORM_MacOSX && !defined(SWEET)\n#define ZONE_MAP_MAX')
edit('osfmk/vm/vm_compressor.c',
     '#if XNU_TARGET_OS_OSX\n\n\tif (vm_compression_limit == 0)',
     '#if XNU_TARGET_OS_OSX && !defined(SWEET)\n\n\tif (vm_compression_limit == 0)')

# Report the complete fixed-claim budget before deriving pointer/data ranges.
# Keep the allocator's placement checks and randomization intact.
edit('osfmk/vm/vm_kern.c',
     '\tassert((total_claims & PAGE_MASK) == 0);\n\tlargest_free_size -= total_claims;',
     '''\tassert((total_claims & PAGE_MASK) == 0);
\tprintf("GM9 VM: largest free=%llu MiB fixed claims=%llu MiB\\n",
\t    (unsigned long long)(largest_free_size >> 20),
\t    (unsigned long long)(total_claims >> 20));
\tfor (uint32_t i = 0; i < kmem_claim_count; i++) {
\t\tprintf("GM9 VM claim %s: %llu MiB\\n", kmem_claims[i].kc_name,
\t\t    (unsigned long long)(kmem_claims[i].kc_size >> 20));
\t}
\tif (total_claims >= largest_free_size) {
\t\tpanic("GM9 VM: fixed claims exceed available kernel VA");
\t}
\tlargest_free_size -= total_claims;''')

# Match the real two-cluster MPIDRs. Keep logical CPU 0 on the boot A73.
edit('iokit/Kernel/arm/SweetCPU.cpp',
     'cpu->phys_id > 0x700 || (cpu->phys_id & 0xff) != 0',
     'cpu->phys_id > 0x103 || (cpu->phys_id & 0xff) > 3')
edit('iokit/Kernel/arm/SweetCPU.cpp',
     'topology->boot_cpu->cpu_id != 0 || topology->boot_cpu->phys_id != 0',
     'topology->boot_cpu->cpu_id != 0 || topology->boot_cpu->phys_id != 0x100')
edit('iokit/Kernel/arm/SweetCPU.cpp', 'expected boot MPIDR 0 and logical CPU 0',
     'expected boot MPIDR 0x100 and logical CPU 0')
edit('iokit/Kernel/arm/SweetCPU.cpp', 'cpu->phys_id == 0 ||',
     'cpu == ml_get_topology_info()->boot_cpu ||')

# Physical RAM above 4 GiB must not be truncated before the page-number shift.
edit('osfmk/arm/pmap/pmap.c', '(vm_offset_t) (pp << PAGE_SHIFT)',
     '((vm_offset_t)pp << PAGE_SHIFT)')

# 4 KiB hardware tables are required by the A53 cluster. Keep XNU's 16 KiB
# software page ABI in hybrid mode so signed iPhone binaries remain native.
edit('osfmk/arm/arm_init.c',
     'if (args->memSize > 1ULL * 1024 * 1024 * 1024)',
     'if (1 /* GM9: keep the 16 KiB software page ABI on 4 KiB hardware */)' if hybrid_pages
     else 'if (0 /* GM9: retain native 4 KiB software pages at every RAM size */)')
if not hybrid_pages:
    edit('osfmk/arm/arm_init.c', 'page_shift_user32 = PAGE_MAX_SHIFT;',
         'page_shift_user32 = ARM_PGSHIFT;')
    edit('bsd/kern/mach_loader.c', 'vm_map_set_page_shift(map, SIXTEENK_PAGE_SHIFT);',
         'vm_map_set_page_shift(map, FOURK_PAGE_SHIFT);')
    p = source / 'osfmk/arm/commpage/commpage.c'
    s = p.read_text(encoding='utf-8')
    for name in ('64_LEGACY', '64'):
        old = f'(_COMM_PAGE_USER_PAGE_SHIFT_{name} + '
        pattern = r'(' + re.escape(old) + r'[^\n]+?\(uint8_t\) )SIXTEENK_PAGE_SHIFT;'
        s, count = re.subn(pattern, r'\g<1>FOURK_PAGE_SHIFT;', s)
        if count != 1:
            raise RuntimeError(f'commpage {name}: unexpected source')
    p.write_bytes(s.encode())
    edit('osfmk/vm/vm_shared_region.c',
         'vm_map_set_page_shift(sub_map, SIXTEENK_PAGE_SHIFT);',
         'vm_map_set_page_shift(sub_map, FOURK_PAGE_SHIFT);')
else:
    edit('osfmk/arm/pmap/pmap.c',
         '\t\tint text_leaf_index = random() % num_ptes;',
         '\t\tint text_leaf_index = (int)(random() % (num_ptes / PAGE_RATIO)) * PAGE_RATIO;')

# The 4K low-globals region starts at -64 GiB, not VM_MIN_KERNEL_ADDRESS
# (-128 GiB). Its L2 table already exists when the UART alias is installed.
edit('osfmk/arm64/arm_vm_init.c',
     '#define SWEET_EARLY_FB_VA (VM_MIN_KERNEL_ADDRESS + ARM_TT_L2_SIZE)',
     '#define SWEET_EARLY_FB_VA (LOW_GLOBAL_BASE_ADDRESS + ARM_TT_L2_SIZE)')

# Two roots, RAM L2 tables, and a separate UART L2 table. Clear the full pool.
edit('osfmk/arm/pmap/pmap.h',
     '#define BOOTSTRAP_TABLE_SIZE (ARM_PGBYTES * 8)',
     '#define BOOTSTRAP_TABLE_SIZE (ARM_PGBYTES * 16)')
edit('osfmk/arm64/genassym.c', 'DECLARE("PGBYTES", ARM_PGBYTES);',
     'DECLARE("PGBYTES", ARM_PGBYTES);\n\tDECLARE("BOOTSTRAP_TABLE_BYTES", BOOTSTRAP_TABLE_SIZE);')
p = source / 'osfmk/arm64/start.s'
s = p.read_text(encoding='utf-8')
s, n = re.subn(r'\tmov\s+x2, #\(TTE_PGENTRIES\)[^\n]*\n\tlsl\s+x2, x2, #2[^\n]*',
    '\tmov x2, #(BOOTSTRAP_TABLE_BYTES / ARM_PGBYTES)\n\tlsl x2, x2, #(ARM_PGSHIFT - TTE_SHIFT)', s)
if n != 1:
    raise RuntimeError('bootstrap clear loop changed')
begin = s.index('\t/*\n\t * Splash framebuffer:')
end = s.index('#endif', begin)
# RAM is above 4 GiB; UART has a different 1 GiB L1 index with 4K tables.
s = s[:begin] + '''\t/* Allocate the UART L2 separately from the upper-bank RAM map. */
\tadd x2, x2, PGBYTES
\tadd x10, x25, #(BOOTSTRAP_TABLE_BYTES >> 12), lsl #12
\tcmp x2, x10
\tb.hs Lgm9_tables_exhausted
\tMOV64 x10, 0x0c000000
\tcreate_l1_table_entry x10, x1, x2, x11, x12, x13
\tMOV64 x11, (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID | ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE) | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX)
\torr x11, x11, x10
\tand x12, x10, #(ARM_TT_L2_INDEX_MASK)
\tlsr x12, x12, #(ARM_TT_L2_SHIFT)
\tstr x11, [x2, x12, lsl #TTE_SHIFT]
\tb Lgm9_tables_ready
Lgm9_tables_exhausted:
\tb Lgm9_tables_exhausted
Lgm9_tables_ready:
''' + s[end:]
old = '3:\tadd\t$4, $4, PGBYTES'
if s.count(old) != 1:
    raise RuntimeError('bootstrap allocator changed')
s = s.replace(old, old + '\n\tadd $6, x25, #(BOOTSTRAP_TABLE_BYTES >> 12), lsl #12\n\tcmp $4, $6\n\tb.hs Lgm9_tables_exhausted')
p.write_bytes(s.encode())
if not hybrid_pages:
    # Per-object timing belongs with the native 4K signature cache patch.
    edit('bsd/kern/ubc_subr.c', '#include <kern/thread.h>',
         '#include <kern/thread.h>\n#include <kern/clock.h>')
    edit('bsd/kern/ubc_subr.c', '\tuint64_t limit, offset, chunk;',
         '\tuint64_t limit, offset, chunk;\n\tuint64_t read_ticks = 0, hash_ticks = 0;')
    edit('bsd/kern/ubc_subr.c',
         '\tioflags = limit > CS_NATIVE4K_UNCACHED ? IO_NOCACHE : 0;',
         '''\tioflags = limit > CS_NATIVE4K_UNCACHED ? IO_NOCACHE : 0;
\tif (limit > CS_NATIVE4K_UNCACHED) {
\t\tprintf("GM9 CS4K begin: pid=%d bytes=%llu base=%lld\\n",
\t\t    proc_selfpid(), limit, (long long)blob->csb_base_offset);
\t}''')
    edit('bsd/kern/ubc_subr.c',
         '\t\tos_atomic_add(&cs_native4k_read_ticks, hash_start - read_start, relaxed);',
         '\t\tread_ticks += hash_start - read_start;\n\t\tos_atomic_add(&cs_native4k_read_ticks, hash_start - read_start, relaxed);')
    edit('bsd/kern/ubc_subr.c',
         '\t\tos_atomic_add(&cs_native4k_hash_ticks, mach_absolute_time() - hash_start, relaxed);',
         '''\t\tconst uint64_t hash_elapsed = mach_absolute_time() - hash_start;
\t\thash_ticks += hash_elapsed;
\t\tos_atomic_add(&cs_native4k_hash_ticks, hash_elapsed, relaxed);
\t\tif (limit > CS_NATIVE4K_UNCACHED &&
\t\t    ((chunk + chunk_length) % (256ULL * 1024 * 1024) == 0)) {
\t\t\tprintf("GM9 CS4K progress: pid=%d bytes=%llu/%llu\\n",
\t\t\t    proc_selfpid(), chunk + chunk_length, limit);
\t\t}''')
    edit('bsd/kern/ubc_subr.c',
         '\tos_atomic_inc(&cs_native4k_ready_blobs, relaxed);\n\terror = 0;',
         '''\tos_atomic_inc(&cs_native4k_ready_blobs, relaxed);
\tif (limit > CS_NATIVE4K_UNCACHED) {
\t\tuint64_t read_ns, hash_ns;
\t\tabsolutetime_to_nanoseconds(read_ticks, &read_ns);
\t\tabsolutetime_to_nanoseconds(hash_ticks, &hash_ns);
\t\tprintf("GM9 CS4K done: pid=%d bytes=%llu read_ms=%llu hash_ms=%llu\\n",
\t\t    proc_selfpid(), limit, read_ns / 1000000ULL, hash_ns / 1000000ULL);
\t}
\terror = 0;''')
print(f'GM9 Pro {mode}: 4 KiB hardware granule, common A53/A73 ISA and eight-CPU topology staged')
