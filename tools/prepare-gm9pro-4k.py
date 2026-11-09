#!/usr/bin/env python3
"""Opt-in GM9 Pro 4 KiB/SMP overrides, after the regular device preparation."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1])

def edit(path, old, new):
    p = source / path
    text = p.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f'{path}: expected one occurrence of {old!r}')
    p.write_text(text.replace(old, new), newline='\n')

edit('pexpert/pexpert/arm64/SWEET.h',
     '#define __ARM_16K_PG__            1',
     '/* Native 4 KiB: leave __ARM_16K_PG__ undefined (proc_reg.h uses ifdef). */\n#define __ARM64_PMAP_SUBPAGE_L1__ 1')
edit('makedefs/MakeInc.def', '-mcpu=cortex-a73', '-mcpu=cortex-a53')

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

# Native 4K page policy adapted from UniXNU's native-4k-user-abi patch.
edit('osfmk/arm/arm_init.c',
     'if (args->memSize > 1ULL * 1024 * 1024 * 1024)', 'if (0 /* GM9: retain native 4 KiB at every RAM size */)')
edit('osfmk/arm/arm_init.c', 'page_shift_user32 = PAGE_MAX_SHIFT;',
     'page_shift_user32 = ARM_PGSHIFT;')
edit('bsd/kern/mach_loader.c', 'vm_map_set_page_shift(map, SIXTEENK_PAGE_SHIFT);',
     'vm_map_set_page_shift(map, FOURK_PAGE_SHIFT);')
p = source / 'osfmk/arm/commpage/commpage.c'
s = p.read_text()
for name in ('64_LEGACY', '64'):
    old = f'(_COMM_PAGE_USER_PAGE_SHIFT_{name} + '
    pattern = r'(' + re.escape(old) + r'[^\n]+?\(uint8_t\) )SIXTEENK_PAGE_SHIFT;'
    s, count = re.subn(pattern, r'\g<1>FOURK_PAGE_SHIFT;', s)
    if count != 1:
        raise RuntimeError(f'commpage {name}: unexpected source')
p.write_text(s, newline='\n')
edit('osfmk/vm/vm_shared_region.c',
     'vm_map_set_page_shift(sub_map, SIXTEENK_PAGE_SHIFT);',
     'vm_map_set_page_shift(sub_map, FOURK_PAGE_SHIFT);')

# Two roots, RAM L2 tables, and a separate UART L2 table. Clear the full pool.
edit('osfmk/arm/pmap/pmap.h',
     '#define BOOTSTRAP_TABLE_SIZE (ARM_PGBYTES * 8)',
     '#define BOOTSTRAP_TABLE_SIZE (ARM_PGBYTES * 16)')
edit('osfmk/arm64/genassym.c', 'DECLARE("PGBYTES", ARM_PGBYTES);',
     'DECLARE("PGBYTES", ARM_PGBYTES);\n\tDECLARE("BOOTSTRAP_TABLE_BYTES", BOOTSTRAP_TABLE_SIZE);')
p = source / 'osfmk/arm64/start.s'
s = p.read_text()
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
p.write_text(s, newline='\n')
print('GM9 Pro native 4 KiB, common A53/A73 ISA and eight-CPU topology staged')
