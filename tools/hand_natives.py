#!/usr/bin/env python3
"""ketsui-nx (2026-10-27, the optimisation round; docs/hand-natives.md of bejeweled3-nx): the hand-written natives of
the five hottest functions of the PC profile (app/src/cpu/hot/localized.txt's first five), made by transforming each
one's localized copy (app/generated/hot/n_<ADDR>.h, its reference and fallback) as bejeweled3-nx's
tools/hand_native_824D8FF8.py does. Writes app/src/cpu/hot/h_<ADDR>.h; tools/localize_native.py then gives the hook
to the hand-written native (the copy stays the reference).

The transforms, and why each keeps the copy's every observable effect (guest stores: the same addresses, bytes and
order; the context registers the copy writes back, with their values; the calls, in order; the same FP operations):

  forward   Stack store-to-load forwarding. The generated code moves integers to floats and back through the stack
            (`std rN,K(r1)` then `lfd fM,K(r1)`; `stfs` then `lwz`): a load of a stack slot that the same run of
            straight-line code stored just before takes the stored value instead (the same bits: the store writes
            them big-endian and the load reads them back big-endian). The STORE STAYS, so memory is as before. A run
            ends at every label, goto, call, sync macro, return and at every store that is not one of r1 with a
            constant offset (it could reach the slot); a store to an overlapping slot replaces its entry, and an
            assignment to the stored local before the load cancels it. Widths: the same width, and the low word of a
            doubleword (lwz K+4 after std K).
  dispatch  sub_82C0E4E8 (XAudio2's linear resampler, one call per channel buffer): the inner loop's per-sample format
            dispatch, `mtctr r26; cmpwi cr6,r26,0; bdzf eq x5; bne` (six compare-and-branches on r26, which the inner
            loop never writes), becomes one switch on r26 with the same outcome: format 1-5 -> its block with ctr = 0,
            format 0 -> the u8 block with ctr = r26 - 5, anything else -> the s32 block with ctr = r26 - 5 (the bdzf
            chain's value). cr6 stays computed as before (every block's path recomputes it before it is read).

  python tools/hand_natives.py [ADDR ...]       (default: the five)
  python tools/hand_natives.py --all          (every copy of app/generated/hot with something to forward)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOT = os.path.join(ROOT, 'app', 'generated', 'hot')
OUT = os.path.join(ROOT, 'app', 'src', 'cpu', 'hot')
TOP5 = ['828C8A28', '82C09230', '82C0E4E8', '82C0D218', '82C08798']

STORE_R1 = re.compile(r'^\s*(?:REX|KETSUI_LOG)_STORE_D_U(32|64)\((l{1,2}_r1)\.u32, (-?\d+), ([\w.]+)\);\s*$')
LOAD_R1 = re.compile(r'^(\s*)([\w.]+) = REX_LOAD_D_U(32|64)\((l{1,2}_r1)\.u32, (-?\d+)\);\s*$')
ANY_STORE = re.compile(r'\bREX_STORE|_LOG_STORE|simde_mm_store_si128\(\(simde__m128i\*\)REX_RAW_ADDR|\bmemset\(|\bmemcpy\(')
BARRIER = re.compile(r'^\s*(loc_\w+:|goto |\{|\}|if \(|return|KETSUI_SYNC|REX_LOOP_(IN|OUT)|.*\(ctx, base\))')


def forward(lines):
    """Stack store-to-load forwarding (see the docstring). Returns the new lines and the count."""
    out, pending, n = [], {}, 0
    for line in lines:
        m = LOAD_R1.match(line)
        if m:
            indent, dest, width, reg, off = m.group(1), m.group(2), int(m.group(3)), m.group(4), int(m.group(5))
            hit = None
            e = pending.get((reg, off))
            if e and e[0] == width:
                hit = e[1]
            elif width == 32:
                e = pending.get((reg, off - 4))
                if e and e[0] == 64:  # lwz K+4 after std K: the doubleword's low word
                    hit = '(uint32_t)(' + e[1] + ')'
            if hit is not None:
                out.append(f'{indent}{dest} = {hit};  // ketsui-nx hand native: forwarded from the stack store above')
                n += 1
                # The destination changed: entries holding it are no longer the stored values.
                base = dest.split('.')[0]
                pending = {k: v for k, v in pending.items() if v[1].split('.')[0] != base}
                continue
            out.append(line)
            base = dest.split('.')[0]
            pending = {k: v for k, v in pending.items() if v[1].split('.')[0] != base}
            continue
        m = STORE_R1.match(line)
        if m:
            width, reg, off, value = int(m.group(1)), m.group(2), int(m.group(3)), m.group(4)
            size = width // 8
            # An overlapping entry goes.
            pending = {k: v for k, v in pending.items()
                       if not (k[0] == reg and k[1] < off + size and off < k[1] + v[0] // 8)}
            pending[(reg, off)] = (width, value)
            out.append(line)
            continue
        if BARRIER.match(line) or ANY_STORE.search(line):
            pending = {}
            out.append(line)
            continue
        # An assignment to a stored local cancels its entries; so does any macro call that names it (it may write it:
        # REX_FABS(l_f1, ...) and the like).
        for mm in re.finditer(r'\b(l{1,2}_\w+|temp|fb\d+)\.\w+\s*(?:=(?!=)|\+=|-=|\|=|&=|\^=|<<=|>>=)', line):
            name = mm.group(1)
            pending = {k: v for k, v in pending.items() if v[1].split('.')[0] != name}
        for mm in re.finditer(r'\bREX_\w+\(([^;]*)', line):
            args = mm.group(1)
            pending = {k: v for k, v in pending.items() if not re.search(r'\b' + re.escape(v[1].split('.')[0]) + r'\b', args)}
        out.append(line)
    return out, n


DISPATCH = [
    '\tctr.u64 = ll_r26.u64;',
    '\tcr6.compare<int32_t>(ll_r26.s32, 0, xer);',
    '\t--ctr.u64;',
    '\tif (ctr.u32 == 0 && !cr6.eq) goto loc_82C0E858;',
    '\t--ctr.u64;',
    '\tif (ctr.u32 == 0 && !cr6.eq) goto loc_82C0E894;',
    '\t--ctr.u64;',
    '\tif (ctr.u32 == 0 && !cr6.eq) goto loc_82C0E9BC;',
    '\t--ctr.u64;',
    '\tif (ctr.u32 == 0 && !cr6.eq) goto loc_82C0E928;',
    '\t--ctr.u64;',
    '\tif (ctr.u32 == 0 && !cr6.eq) goto loc_82C0EA00;',
    '\tif (!cr6.eq) goto loc_82C0EA44;',
]
SWITCH = [
    '\t// ketsui-nx hand native: the six compare-and-branches of the format dispatch as one switch (same outcome).',
    '\tcr6.compare<int32_t>(ll_r26.s32, 0, xer);',
    '\tswitch (ll_r26.u32) {',
    '\tcase 1: ctr.u64 = 0; goto loc_82C0E858;',
    '\tcase 2: ctr.u64 = 0; goto loc_82C0E894;',
    '\tcase 3: ctr.u64 = 0; goto loc_82C0E9BC;',
    '\tcase 4: ctr.u64 = 0; goto loc_82C0E928;',
    '\tcase 5: ctr.u64 = 0; goto loc_82C0EA00;',
    '\tcase 0: ctr.u64 = ll_r26.u64 - 5; break;',
    '\tdefault: ctr.u64 = ll_r26.u64 - 5; goto loc_82C0EA44;',
    '\t}',
]


def dispatch_82C0E4E8(lines):
    for i in range(len(lines) - len(DISPATCH) + 1):
        if lines[i:i + len(DISPATCH)] == DISPATCH and i >= 3 and lines[i - 3].startswith('loc_82C0E7FC:'):
            return lines[:i] + SWITCH + lines[i + len(DISPATCH):], 1
    sys.exit('hand_natives: sub_82C0E4E8: the format dispatch is not in the copy as expected (the copy changed?)')


SPECIFIC = {'82C0E4E8': dispatch_82C0E4E8}


def make(addr):
    src = os.path.join(HOT, f'n_{addr}.h')
    text = open(src, encoding='utf-8').read()
    start = text.index(f'namespace ketsui::hot::n_{addr} {{')
    end = text.index(f'}}  // namespace ketsui::hot::n_{addr}')
    body = text[start:end].replace(f'namespace ketsui::hot::n_{addr} {{', f'namespace ketsui::hot::h_{addr} {{', 1)
    leaf = 'inline void NativeLogged' in body  # both variants get the same transforms; the guard checks the logged one
    lines = body.split('\n')
    lines, forwarded = forward(lines)
    path = os.path.join(OUT, f'h_{addr}.h')
    if not forwarded and addr not in SPECIFIC:
        print(f'sub_{addr}: nothing to forward: no hand native')
        if os.path.exists(path):
            os.remove(path)
        return False
    notes = [f'stack store-to-load forwarding at {forwarded} load(s)' + (' (both variants)' if leaf else '')]
    if addr in SPECIFIC:
        lines, k = SPECIFIC[addr](lines)
        notes.append('the inner loop\'s format dispatch as one switch')
    what = text.split('\n', 1)[0].split(':', 1)[1].strip().rstrip('.')
    head = (f'// h_{addr}.h: hand-written native of sub_{addr} ({what}). Reference: app/generated/hot/n_{addr}.h.\n'
            f'// MADE by tools/hand_natives.py from that copy (do not edit; run the tool again after the copy changes).\n'
            f'// Optimisations: {"; ".join(notes)}.\n'
            f'// Audit: tools/hand_natives.py\'s docstring: every guest store, context write-back, call and FP operation of\n'
            f'// the copy stays (the forwarded loads read back what the stores just above wrote; the dispatch has the bdzf\n'
            f'// chain\'s outcome and ctr value). '
            + ('A leaf: its guard checks NativeLogged against the original.' if leaf
               else 'Unguarded, as its copy (it calls other functions).') + '\n#pragma once\n\n')
    out = head + '\n'.join(lines) + f'}}  // namespace ketsui::hot::h_{addr}\n'
    os.makedirs(OUT, exist_ok=True)
    old = open(path, encoding='utf-8').read() if os.path.exists(path) else None
    if old != out:
        open(path, 'w', encoding='utf-8', newline='\n').write(out)
    print(f'sub_{addr}: {"; ".join(notes)} -> {os.path.relpath(path, ROOT)}')
    return True


def main():
    args = sys.argv[1:]
    if args == ['--all']:
        # The five first, then every other localized copy that has stack round trips.
        import glob
        others = {os.path.basename(p)[2:10] for p in glob.glob(os.path.join(HOT, 'n_*.h'))} - set(TOP5)
        args = TOP5 + sorted(others)
    made = sum(1 for addr in (args or TOP5) if make(addr.upper().replace('SUB_', '')))
    print(f'{made} hand-written natives in {os.path.relpath(OUT, ROOT)}')


if __name__ == '__main__':
    main()
