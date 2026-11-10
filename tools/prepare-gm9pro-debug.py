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
print('GM9 bounded boot-wait sampler staged (gm9sample=1)')
