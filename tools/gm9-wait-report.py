#!/usr/bin/env python3
"""Summarize GM9 WAIT UART samples; optionally resolve a matching dyld cache.

Usage: gm9-wait-report.py UART.txt [--cache dyld_shared_cache_arm64] [--ipsw PATH]
Thread indices are local to each snapshot, not stable thread identifiers.
"""
import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess

HEADER = re.compile(r'GM9 WAIT (\S+)\[(\d+)\] threads=(\d+) blocked_shown=(\d+) '
                    r'slide=(0x[0-9a-f]+) mh=(0x[0-9a-f]+)')
THREAD = re.compile(r'GM9 WAIT (\S+)\[(\d+)\] t(\d+) state=([0-9a-f]+) '
                    r'event=([0-9a-f]+) pc=([0-9a-f]+) lr=([0-9a-f]+) '
                    r'sp=([0-9a-f]+) bt((?: [0-9a-f]+)*)')
FRAME = re.compile(r'GM9 FRAME ticks=(\d+) swaps=(\d+) copies=(\d+) notify=(\d+) '
                   r'delivered=(\d+) errors=(\d+) last=(0x[0-9a-f]+)')


def samples(text):
    current = 'unknown'
    for line in text.splitlines():
        if 'GM9 WAIT snapshot=' in line:
            current = line[line.index('GM9 WAIT snapshot='):]
        frame = FRAME.search(line)
        if frame:
            keys = ('ticks', 'swaps', 'copies', 'notify', 'delivered', 'errors', 'last')
            values = frame.groups()
            row = dict(zip(keys, [int(v) for v in values[:-1]] + [int(values[-1], 16)]))
            yield current, None, None, None, None, None, None, row
            continue
        header = HEADER.search(line)
        if header:
            name, pid, total, shown, slide, mh = header.groups()
            yield current, name, int(pid), int(total), int(shown), int(slide, 16), int(mh, 16), None
            continue
        thread = THREAD.search(line)
        if thread:
            name, pid, index, state, event, pc, lr, sp, bt = thread.groups()
            row = dict(index=int(index), state=int(state, 16), event=int(event, 16),
                       pc=int(pc, 16), lr=int(lr, 16), sp=int(sp, 16),
                       bt=tuple(int(v, 16) for v in bt.split()))
            yield current, name, int(pid), None, None, None, None, row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--cache')
    parser.add_argument('--ipsw', default='ipsw')
    args = parser.parse_args()
    contexts, resolved, repeats = {}, {}, Counter()

    def address(value, slide, mh):
        if not value:
            return '0'
        if mh <= value < mh + 0x10000000:
            return f'main+0x{value - mh:x}'
        unslid = value - slide
        if not 0x180000000 <= unslid < 0x300000000:
            return hex(value)
        if not args.cache:
            return f'cache:0x{unslid:x}'
        if unslid not in resolved:
            result = subprocess.run([args.ipsw, 'dyld', 'a2s', args.cache, hex(unslid)],
                                    capture_output=True, text=True, check=False)
            matches = [line for line in result.stdout.splitlines() if line.startswith('0x')]
            resolved[unslid] = matches[-1].split(': ', 1)[-1] if matches else f'cache:0x{unslid:x} (unresolved)'
        return resolved[unslid]

    headers = 0
    previous_frame = None
    for snapshot, name, pid, total, shown, slide, mh, row in samples(
            args.log.read_text(encoding='utf-8', errors='replace')):
        if name is None:
            print(f'\n{snapshot}: display counters {row}')
            if previous_frame is not None and row['ticks'] >= previous_frame['ticks']:
                print('  delta ' + ', '.join(f'{key}=+{row[key] - previous_frame[key]}'
                      for key in ('ticks', 'swaps', 'copies', 'notify', 'delivered', 'errors')))
            previous_frame = row
            continue
        if row is None:
            headers += 1
            contexts[pid] = slide, mh
            print(f'\n{snapshot}: {name}[{pid}] threads={total}, blocked shown={shown}')
            continue
        if pid not in contexts:
            print(f'  {name}[{pid}] sample missing its header; addresses left unresolved')
            continue
        slide, mh = contexts[pid]
        stack = (row['pc'], row['lr']) + row['bt']
        repeats[(name, pid, tuple(value - slide for value in stack))] += 1
        print(f'  t{row["index"]} state=0x{row["state"]:x} event=0x{row["event"]:x}')
        for value in stack:
            print('    ' + address(value, slide, mh))
    if not headers:
        parser.exit(1, 'No complete GM9 WAIT headers found in this log.\n')
    print('\nRepeated stack observations (indices may refer to different threads):')
    for (name, pid, _), count in repeats.most_common(12):
        if count > 1:
            print(f'  {name}[{pid}]: {count}')


if __name__ == '__main__':
    main()
