#!/usr/bin/env python3
"""Whole-tick differential test: the ORIGINAL UpdateEntities (0x41780) on the port's memory snapshot vs.
the port's own result of the same tick.

1. remc1.exe ... --snapshot-frame N --snapshot-dir D   (D/before.bin, D/after.bin, D/meta.txt)
2. python tools/diff_tick.py D

Differences are listed per entity (class/model/state, field offsets). Narrow them down per handler
with tools/diff_funcs.py (runs original and port code side by side on the same state).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from emu import Machine

ENTITY_BASE, ENTITY_SIZE, ENTITY_COUNT = 29795, 164, 1000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('snapdir')
    ap.add_argument('--show', type=int, default=60)
    a = ap.parse_args()
    m = Machine(a.snapdir, load_port_code=False)
    st, ss = m.state_addr, m.state_size
    org = bytes(m.mu.mem_read(st, ss))
    m.call(0x41780, [], max_insns=500_000_000)
    if m.stop_reason:
        print('STOPPED:', m.stop_reason)
        return 2
    emu = bytes(m.mu.mem_read(st, ss))
    prt = m.after.read(st, ss)
    diffs = []
    for i in range(ss):
        if emu[i] == prt[i]:
            continue
        # pointer written as an original address by the original and as a port address by the port
        ok = False
        for s in range(i - 3, i + 1):
            if 0 <= s and s + 4 <= ss:
                ov, pv = struct.unpack_from('<I', emu, s)[0], struct.unpack_from('<I', prt, s)[0]
                if ov != pv and m.translate_value(pv) == ov:
                    ok = True
                    break
        if not ok:
            diffs.append(i)
    print(f'game state: {len(diffs)} bytes differ between original and port '
          f'(original changed {sum(1 for i in range(ss) if emu[i] != org[i])}, port changed {sum(1 for i in range(ss) if prt[i] != org[i])})')
    by_entity, other = {}, []
    for i in diffs:
        if ENTITY_BASE <= i < ENTITY_BASE + ENTITY_SIZE * ENTITY_COUNT:
            e, f = divmod(i - ENTITY_BASE, ENTITY_SIZE)
            by_entity.setdefault(e, []).append(f)
        else:
            other.append(i)
    for e, fields in sorted(by_entity.items())[:a.show]:
        eb = ENTITY_BASE + ENTITY_SIZE * e
        fs = sorted(set(f & ~1 for f in fields))
        desc = ', '.join(f'+{f}: before {struct.unpack_from("<H", org, eb + f)[0]} orig {struct.unpack_from("<H", emu, eb + f)[0]} '
                         f'port {struct.unpack_from("<H", prt, eb + f)[0]}' for f in fs[:6])
        print(f'  entity {e} (class {org[eb + 64]} model {org[eb + 65]} state {org[eb + 70]}): {desc}{" ..." if len(fs) > 6 else ""}')
    if other:
        print('  outside the entity array: ' + ', '.join(f'+{i}: orig {emu[i]} port {prt[i]}' for i in other[:30]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
