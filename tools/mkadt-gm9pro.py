#!/usr/bin/env python3
"""Single big-CPU ADT for the first GM9 Pro XNU entry test."""
from pathlib import Path
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
setprop(chosen, 'dram-size', struct.pack('<Q', 0x20000000))
cpus = next(n for n in root.children if n.name == 'cpus')
cpu = cpus.children[0]
setprop(cpu, 'reg', struct.pack('<I', 0x100))
for key in ('timebase-frequency', 'peripheral-frequency', 'fixed-frequency'):
    setprop(cpu, key, struct.pack('<I', 19200000))
armio = next(n for n in root.children if n.name == 'arm-io')
gic = next(n for n in armio.children if n.name == 'gic')
setprop(gic, 'reg', struct.pack('<4Q', 0x17a00000 - mkadt.SOC_BASE, 0x10000,
                              0x17b00000 - mkadt.SOC_BASE, 0x100000))
setprop(gic, 'virtual-timer-intid', struct.pack('<I', 19))
setprop(gic, 'redistributor-stride', struct.pack('<I', 0x20000))
root.children = [n for n in root.children if n.name not in ('sweet-fbcon', 'sweet-ufs')]
armio.children = [n for n in armio.children if n.name != 'disp0']
Path(sys.argv[1]).write_bytes(adt.serialize(root))
print('GM9 Pro ADT: CPU 0x100, GIC 0x17a00000/0x17b00000, UART diagnostics')
