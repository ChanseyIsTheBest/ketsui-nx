#!/usr/bin/env python3
"""ketsui-nx (2026-10-27, the optimisation round): the candidates for localized copies, from a PC profile's guest
functions (logs/pc_profile.log, ketsui_pc_profile_s with ketsui_pc_profile_top = 400): ranked by self samples over the
chosen windows, without what docs/optimising-functions.md (bejeweled3-nx) leaves out:

  - tiny leaves (12 guest instructions or fewer and no call): the generator inlines them already;
  - functions with atomics (lwarx / stwcx.) or the global lock: tools/localize_native.py refuses them;
  - any address the app or SDK sources mention (their hooks: two hooks on one function do not link);
  - functions larger than --max-ins guest instructions (a localized copy of a whole state machine is no gain).

  python tools/hot_candidates.py out/pc_profile_round1.log [--from-report 3] [--top 100] [--exclude ADDR ...]

Prints "<ADDR> <samples> <instructions> <calls> <leaf|calls>" lines, best first.
"""
import argparse
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_codegen_post  # noqa: E402  (its hooked(): every address the sources mention)
from d3d_match import function_starts  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOW = re.compile(r'^== pc profile window')
THREAD = re.compile(r'^thread \d+ "(.*)": busy')
LINE = re.compile(r'^\s+[\d.]+%\s+\S*!sub_([0-9A-F]{8}) x(\d+)$')
ATOMIC = re.compile(r'\b(lwarx|stwcx|ldarx|stdcx)\b')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--from-report', type=int, default=3)
    ap.add_argument('--top', type=int, default=100)
    ap.add_argument('--max-ins', type=int, default=6000)
    ap.add_argument('--exclude', nargs='*', default=[])
    a = ap.parse_args()
    samples = collections.Counter()
    window = 0
    for line in open(a.log, encoding='utf-8', errors='replace'):
        if WINDOW.match(line):
            window += 1
            continue
        if window < a.from_report:
            continue
        m = LINE.match(line.rstrip())
        if m:
            samples[m.group(1)] += int(m.group(2))
    gen = os.path.join(ROOT, 'app', 'generated', 'default')
    starts = function_starts(gen)
    size = {}
    for i, s in enumerate(starts):
        size['%08X' % s] = ((starts[i + 1] if i + 1 < len(starts) else s + 4) - s) // 4
    image = open(os.path.join(ROOT, 'out', 'default_image.bin'), 'rb').read()
    hooked = {n[4:] for n in perf_codegen_post.hooked()}
    exclude = {x.upper().replace('SUB_', '') for x in a.exclude}
    out = []
    skipped = collections.Counter()
    for addr, n in samples.most_common():
        ins = size.get(addr)
        if ins is None:
            skipped['not a function start'] += 1
            continue
        start = int(addr, 16) - 0x82000000
        words = [struct.unpack_from('>I', image, start + 4 * k)[0] for k in range(ins)]
        calls = sum(1 for w in words if (w >> 26) == 18 and (w & 1)) + sum(
            1 for w in words if (w >> 26) == 19 and ((w >> 1) & 0x3FF) == 528 and (w & 1))
        atomic = any((w >> 26) == 31 and ((w >> 1) & 0x3FF) in (20, 150, 84, 214) for w in words)
        if addr in exclude:
            skipped['excluded'] += 1
        elif addr in hooked:
            skipped['hooked by the sources'] += 1
        elif ins <= 12 and calls == 0:
            skipped['tiny leaf'] += 1
        elif atomic:
            skipped['atomics'] += 1
        elif ins > a.max_ins:
            skipped['larger than --max-ins'] += 1
        else:
            out.append((addr, n, ins, calls))
        if len(out) >= a.top:
            break
    for addr, n, ins, calls in out:
        print(f'{addr} {n} {ins} {calls} {"leaf" if calls == 0 else "calls"}')
    print(f'# {len(out)} candidates; left out: ' + ', '.join(f'{k} {v}' for k, v in skipped.most_common()),
          file=sys.stderr)


if __name__ == '__main__':
    main()
