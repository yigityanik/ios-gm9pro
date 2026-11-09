#!/usr/bin/env python3
"""Keep the experimental no-passcode KeyStore state internally consistent."""
from pathlib import Path
import sys

p = Path(sys.argv[1]) / 'iokit/Kernel/arm/SweetKeyStore.cpp'
s = p.read_text()
for old, new in (
    ('#define SWEET_AKS_LOCK_STATE    0x4',
     '#define SWEET_AKS_LOCK_STATE    0x6 /* no passcode, unlocked since boot */'),
    ('{ "sls", 0 }', '{ "sls", 3 } /* passcode disabled */'),
):
    if s.count(old) != 1:
        raise RuntimeError(f'KeyStore source changed: {old}')
    s = s.replace(old, new)
begin = s.index('\tchar name[32];\n\tproc_selfname')
end = s.index('\n\treturn ret;', begin)
s = s[:begin] + '''\t/* Bound UART traffic even when userspace repeatedly polls a selector. */
\tstatic uint32_t logged[129];
\tunsigned int bucket = selector < 128 ? selector : 128;
\tif (__atomic_fetch_add(&logged[bucket], 1, __ATOMIC_RELAXED) < 5) {
\t\tchar name[32];
\t\tproc_selfname(name, sizeof(name));
\t\tIOLog("SWEET AKS: %s sel %u arg0 %llu in %u/%u out %u/%u -> 0x%x\\n", name, selector,
\t\t    arg0, in_scalars, in_struct, out_scalars, out_struct, ret);
\t}
''' + s[end:]
p.write_text(s, newline='\n')
print('GM9 KeyStore: consistent no-passcode state and bounded SMP-safe logging')
