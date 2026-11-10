#!/usr/bin/env python3
"""Keep the experimental no-passcode KeyStore state internally consistent."""
from pathlib import Path
import sys

p = Path(sys.argv[1]) / 'iokit/Kernel/arm/SweetKeyStore.cpp'
s = p.read_text(encoding='utf-8')
for old, new in (
    ('#define SWEET_AKS_LOCK_STATE    0x4',
     '#define SWEET_AKS_LOCK_STATE    0x6 /* no passcode, unlocked since boot */'),
    ('{ "sls", 0 }', '{ "sls", 3 } /* passcode disabled */'),
):
    if s.count(old) != 1:
        raise RuntimeError(f'KeyStore source changed: {old}')
    s = s.replace(old, new)
# Keep the notification latch separate from the lock/passcode state.
old = '\tcase kAKSNotifyStateSet:\n\t\targs->scalarOutputCount = 0;'
new = '''\t\targs->scalarOutputCount = 0;
\t\tbreak;
\tcase kAKSNotifyStateSet:
\t\t__atomic_store_n(&sweet_aks_notify_state, 1, __ATOMIC_RELEASE);
\t\targs->scalarOutputCount = 0;'''
if s.count(old) != 1:
    raise RuntimeError('KeyStore notification setter changed')
s = s.replace(old, new)
old = '\t\t/* Nothing was ever set: answer "not set". */'
if s.count(old) != 1:
    raise RuntimeError('KeyStore notification getter changed')
begin = s.index(old)
end = s.index('\n\tcase kAKSGetDeviceState:', begin)
body = s[begin:end].replace(old, '\t\t/* Return the state published by NotifyStateSet. */')
body = body.replace('args->scalarOutput[0] = 0;',
                    'args->scalarOutput[0] = __atomic_load_n(&sweet_aks_notify_state, __ATOMIC_ACQUIRE);')
s = s[:begin] + body + s[end:]
anchor = 'IOReturn\nAppleKeyStoreUserClient::externalMethod'
if s.count(anchor) != 1:
    raise RuntimeError('KeyStore method changed')
s = s.replace(anchor, 'static uint32_t sweet_aks_notify_state;\n\n' + anchor)
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
p.write_bytes(s.encode())
print('GM9 KeyStore: consistent state, notification latch and bounded SMP-safe logging')
