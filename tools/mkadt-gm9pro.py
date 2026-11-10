#!/usr/bin/env python3
"""Single big-CPU ADT for the first GM9 Pro XNU entry test."""
from pathlib import Path
import os
import copy
import struct
import sys
import adt
import mkadt

def setprop(node, name, value):
    node.props = [(n, f, v) for n, f, v in node.props if n != name]
    node.props.append((name, 0, value))

root = mkadt.build(qemu=True)
chosen = next(n for n in root.children if n.name == 'chosen')
# Use only the first 512 MiB of the upper bank from the device's ABL RAM log.
setprop(chosen, 'dram-base', struct.pack('<Q', 0x100000000))
ram = os.environ.get('GM9PRO_RAM', '512m')
sizes = {'512m': 0x20000000, 'upper-bank': 0x7e3c0000,
         '4g-device': 0x17e3c0000 - 0xa0400000}
if ram not in sizes:
    raise SystemExit('GM9PRO_RAM must be 512m, upper-bank or 4g-device')
setprop(chosen, 'dram-size', struct.pack('<Q', sizes[ram]))
if ram == '4g-device':
    # Leave modem, TrustZone, splash and ramoops below this window.
    setprop(chosen, 'dram-base', struct.pack('<Q', 0xa0400000))
cpus = next(n for n in root.children if n.name == 'cpus')
cpu = cpus.children[0]
setprop(cpu, 'reg', struct.pack('<I', 0x100))
for key in ('timebase-frequency', 'peripheral-frequency', 'fixed-frequency'):
    setprop(cpu, key, struct.pack('<I', 19200000))
count = int(os.environ.get('GM9PRO_CPUS', '1'))
if count not in (1, 4, 8):
    raise SystemExit('GM9PRO_CPUS must be 1, 4 or 8')
if count in (4, 8):
    cpus.children = []
    mpidrs = (0x100, 0x101, 0x102, 0x103)
    if count == 8:
        mpidrs += (0, 1, 2, 3)
    for logical, mpidr in enumerate(mpidrs):
        n = copy.deepcopy(cpu)
        setprop(n, 'name', ('cpu%d' % logical).encode() + b'\0')
        setprop(n, 'reg', struct.pack('<I', mpidr))
        setprop(n, 'cpu-id', struct.pack('<I', logical))
        setprop(n, 'AAPL,phandle', struct.pack('<I', 0x1000 + logical))
        setprop(n, 'cluster-core-id', struct.pack('<I', logical))
        setprop(n, 'l2-cache-id', struct.pack('<I', mpidr >> 8))
        setprop(n, 'state', b'running\0' if logical == 0 else b'stopped\0')
        cpus.children.append(n)
armio = next(n for n in root.children if n.name == 'arm-io')
gic = next(n for n in armio.children if n.name == 'gic')
setprop(gic, 'reg', struct.pack('<4Q', 0x17a00000 - mkadt.SOC_BASE, 0x10000,
                              0x17b00000 - mkadt.SOC_BASE, 0x100000))
setprop(gic, 'virtual-timer-intid', struct.pack('<I', 19))
setprop(gic, 'redistributor-stride', struct.pack('<I', 0x20000))
root.children = [n for n in root.children if n.name not in ('sweet-fbcon', 'sweet-ufs')]
Path(sys.argv[1]).write_bytes(adt.serialize(root))
print(f'GM9 Pro ADT: {count} CPU(s), boot MPIDR 0x100, RAM {sizes[ram] // 1048576} MiB')
