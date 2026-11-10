#!/usr/bin/env python3
"""Keep native 16 KiB XNU pages and expose only the four GM9 Pro A73 cores."""
from pathlib import Path
import sys

source = Path(sys.argv[1])

def edit(path, old, new):
    p = source / path
    text = p.read_text(encoding='utf-8')
    if text.count(old) != 1:
        raise RuntimeError(f'{path}: expected one occurrence of {old!r}')
    p.write_text(text.replace(old, new), encoding='utf-8')

# This is a native 16 KiB build. Refuse to proceed if an earlier patch or
# preparation step has removed XNU's hardware-granule selection.
header = (source / 'pexpert/pexpert/arm64/SWEET.h').read_text(encoding='utf-8')
if '#define __ARM_16K_PG__' not in header:
    raise RuntimeError('SWEET.h does not enable the native 16 KiB granule')

# The device reports TGran16 on A73 MPIDRs 0x100..0x103, but not on A53s.
# Make the ADT/XNU topology use those four CPUs only, and keep 0x100 as boot CPU.
edit('iokit/Kernel/arm/SweetCPU.cpp',
     'cpu->phys_id > 0x700 || (cpu->phys_id & 0xff) != 0',
     'cpu->phys_id < 0x100 || cpu->phys_id > 0x103 || (cpu->phys_id & 0xff) > 3')
edit('iokit/Kernel/arm/SweetCPU.cpp',
     'topology->boot_cpu->cpu_id != 0 || topology->boot_cpu->phys_id != 0',
     'topology->boot_cpu->cpu_id != 0 || topology->boot_cpu->phys_id != 0x100')
edit('iokit/Kernel/arm/SweetCPU.cpp', 'cpu->phys_id == 0 ||',
     'cpu == ml_get_topology_info()->boot_cpu ||')

# The 4 GiB GM9 RAM window extends above 4 GiB in physical-address space.
# Widen before shifting PFNs so the intermediate arithmetic cannot truncate.
edit('osfmk/arm/pmap/pmap.c', '(vm_offset_t) (pp << PAGE_SHIFT)',
     '((vm_offset_t)pp << PAGE_SHIFT)')

print('GM9 Pro: native 16 KiB pages, A73-only 4-CPU topology, 4 GiB PFN fix')
