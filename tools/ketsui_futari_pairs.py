#!/usr/bin/env python3
"""Futari's Direct3D (XDK 2.0.9328, the same XDK as Ketsui's) paired with Ketsui's, function by function, and the guest
addresses the pairs use translated: Futari address -> Ketsui address (ketsui-nx, 2026-10-27, for Renut's renderer).

Every Futari function of the given range (default: its Direct3D, 0x82096B28-0x820AE000) is paired with the Ketsui
function of the same instruction shape (tools/d3d_match.py's tokens: immediates and branch displacements left out;
trailing alignment words dropped) when exactly one Ketsui function has it. In each pair, the instructions are walked
together: a lis gives a register its high half, and the addi / load / store off that register gives a global's address;
bl gives a callee. Where Futari's and Ketsui's differ, that is one translation (Futari -> Ketsui), counted.

  python tools/ketsui_futari_pairs.py [--futari ../futari-nx] [--lo 0x82096B28 --hi 0x820AE000]
      [--addr 0x821E1AF8 ...]   only these Futari addresses (globals or functions)
      [--pairs]                 also list every pair
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3d_match import function_starts, token  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000


def load(image_path, gen):
    image = open(image_path, 'rb').read()
    starts = function_starts(gen)
    funcs = {}
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else s + 4
        n = (e - s) // 4
        off = s - BASE
        if off < 0 or off + 4 * n > len(image) or n > 4000:
            continue
        ws = [struct.unpack_from('>I', image, off + 4 * k)[0] for k in range(n)]
        while ws and ws[-1] == 0:
            ws.pop()
        if ws:
            funcs[s] = ws
    return funcs


def shape(ws):
    return tuple(token(w) for w in ws)


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


# D-form ops whose effective address is ra + simm: addi, loads, stores.
EA_OPS = {14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55}


def refs(addr, ws):
    """[(index, kind, value)]: globals (lis base + displacement) and callees, in order."""
    out = []
    hi = {}
    for k, w in enumerate(ws):
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:  # lis rd, imm
            hi[rd] = (w & 0xFFFF) << 16
            continue
        if op in EA_OPS and ra != 0 and ra in hi:
            out.append((k, 'g', (hi[ra] + simm(w)) & 0xFFFFFFFF))
        elif op in (58, 62) and ra in hi:  # ld / std (DS-form)
            out.append((k, 'g', (hi[ra] + simm(w & ~3)) & 0xFFFFFFFF))
        elif op == 18 and (w & 1):  # bl
            d = w & 0x03FFFFFC
            if d & 0x02000000:
                d -= 0x04000000
            out.append((k, 'f', (addr + 4 * k + d) & 0xFFFFFFFF))
        if op not in (24, 25, 26, 27, 28, 29) and rd in hi and op != 15:
            # A register written by anything but lis loses its high half (stores write no register: keep).
            if op not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55, 62):
                hi.pop(rd, None)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--futari', default=os.path.join(ROOT, '..', 'futari-nx'))
    ap.add_argument('--lo', type=lambda v: int(v, 0), default=0x82096B28)
    ap.add_argument('--hi', type=lambda v: int(v, 0), default=0x820AE000)
    ap.add_argument('--addr', type=lambda v: int(v, 0), nargs='*', default=[])
    ap.add_argument('--pairs', action='store_true')
    a = ap.parse_args()
    fut = load(os.path.join(a.futari, 'out', 'default_image.bin'), os.path.join(a.futari, 'app', 'generated', 'default'))
    ket = load(os.path.join(ROOT, 'out', 'default_image.bin'), os.path.join(ROOT, 'app', 'generated', 'default'))
    by_shape = collections.defaultdict(list)
    for s, ws in ket.items():
        by_shape[shape(ws)].append(s)
    pairs = {}
    unpaired = []
    for s, ws in sorted(fut.items()):
        if not a.lo <= s < a.hi:
            continue
        c = by_shape.get(shape(ws), [])
        if len(c) == 1:
            pairs[s] = c[0]
        else:
            unpaired.append((s, len(ws), len(c)))
    trans = collections.defaultdict(collections.Counter)
    for fs, ks in pairs.items():
        trans[fs][ks] += 1  # the function itself
        fr, kr = refs(fs, fut[fs]), refs(ks, ket[ks])
        for (fk, fkind, fv), (kk, kkind, kv) in zip(fr, kr):
            if fk == kk and fkind == kkind:
                trans[fv][kv] += 1
    print('Futari functions in 0x%08X-0x%08X: %d paired, %d not (no or several Ketsui functions of their shape)' %
          (a.lo, a.hi, len(pairs), len(unpaired)))
    if a.pairs:
        for fs, ks in sorted(pairs.items()):
            print('  pair %08X -> %08X (%d instructions)' % (fs, ks, len(fut[fs])))
        for s, n, c in unpaired:
            print('  unpaired %08X (%d instructions, %d candidates)' % (s, n, c))
    want = a.addr or sorted(trans)
    for f in want:
        c = trans.get(f)
        if not c:
            print('%08X: no pair uses it' % f)
            continue
        best = c.most_common()
        print('%08X -> %s' % (f, ', '.join('%08X (%d)' % (k, n) for k, n in best[:4])))


if __name__ == '__main__':
    main()
