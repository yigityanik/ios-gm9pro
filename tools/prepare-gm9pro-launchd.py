#!/usr/bin/env python3
"""Do not advertise an unavailable FairPlay service during hardware bring-up."""
from pathlib import Path
import plistlib
import sys

p = Path(sys.argv[1])
raw = p.read_bytes()
data = plistlib.loads(raw)
jobs = data['LaunchDaemons']
remove = [key for key, job in jobs.items()
          if job.get('Label') == 'com.apple.fairplayd.H2']
for key in remove:
    del jobs[key]
if remove:
    fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
    p.write_bytes(plistlib.dumps(data, fmt=fmt, sort_keys=False))
assert not any(job.get('Label') == 'com.apple.fairplayd.H2' for job in jobs.values())
print(f'FairPlay launch jobs removed: {len(remove)}; other jobs preserved')
