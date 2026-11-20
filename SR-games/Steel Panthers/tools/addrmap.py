#!/usr/bin/env python3
"""Map addresses of one build of STEEL.EXE to another (v1.1 -> v1.2).

Code addresses: bytes are compared with the operands that differ between builds masked (fixup
targets and E8/E9 rel32 displacements).
1. Anchors: every call target (function start) of the old build whose 32-byte window matches
   exactly one place in the new build; only anchors whose order agrees in both builds are kept
   (longest increasing subsequence), which drops matches of look-alike functions.
2. An address is searched only between the new positions of the anchors around it: forward
   windows first, then windows that start before it; finally the shift of the anchor below is
   used if the bytes there match.

Data addresses: every instruction in the old build whose fixup points at (or just below) the
address is mapped as code, and the fixup at the same place in the new build gives the new target;
the answer is the majority vote.

usage: addrmap.py OLD.EXE NEW.EXE [addr ...]       (hex; prints "old new how")
"""
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import le2elf  # noqa: E402


class Build:
    def __init__(self, path):
        info = le2elf.parse(path)
        self.objs = [(o['base'], bytes(o['img'][:o['vsize']])) for o in info['objs']]
        self.code_base, self.code = self.objs[0]
        self.data_base, self.data = self.objs[1]
        self.fix = dict(info['fixups'])      # source address -> target
        self.mask = bytearray(len(self.code))  # 1 = byte differs between builds
        for s in self.fix:
            o = s - self.code_base
            if 0 <= o < len(self.code):
                for k in range(4):
                    if o + k < len(self.mask):
                        self.mask[o + k] = 1
        c = self.code
        self.call_targets = set()
        for o in range(len(c) - 5):
            if c[o] in (0xe8, 0xe9) and not self.mask[o]:
                for k in range(1, 5):
                    self.mask[o + k] = 1
                if c[o] == 0xe8:
                    t = o + 5 + int.from_bytes(c[o + 1:o + 5], 'little', signed=True)
                    if 0 <= t < len(c):
                        self.call_targets.add(self.code_base + t)
        self.by_target = collections.defaultdict(list)
        for s, t in self.fix.items():
            self.by_target[t].append(s)
        self.targets = sorted(self.by_target)

    def window(self, a, n):
        o = a - self.code_base
        if o < 0 or o + n > len(self.code):
            return None
        return self.code[o:o + n], self.mask[o:o + n]


def _lis(pairs):
    """longest subsequence of (old, new) pairs (sorted by old) with increasing new"""
    tails, prev, idx = [], [None] * len(pairs), []
    for i, (_, n) in enumerate(pairs):
        j = bisect.bisect_left(tails, n)
        if j == len(tails):
            tails.append(n)
            idx.append(i)
        else:
            tails[j] = n
            idx[j] = i
        prev[i] = idx[j - 1] if j else None
    out, i = [], idx[-1] if idx else None
    while i is not None:
        out.append(pairs[i])
        i = prev[i]
    return out[::-1]


class Mapper:
    def __init__(self, old, new):
        self.a = Build(old) if isinstance(old, str) else old
        self.b = Build(new) if isinstance(new, str) else new
        cand = []
        for t in sorted(self.a.call_targets):
            w = self.a.window(t, 32)
            if w:
                hits = self._find(*w)
                if len(hits) == 1:
                    cand.append((t, self.b.code_base + hits[0]))
        self.anchors = _lis(cand)
        self.anchor_old = [p[0] for p in self.anchors]
        self.dropped = len(cand) - len(self.anchors)

    def _find(self, pat, msk, lo=0, hi=None):
        """offsets p in [lo, hi) of b.code where pat matches (masked positions ignored)"""
        code, bm = self.b.code, self.b.mask
        if hi is None or hi > len(code):
            hi = len(code)
        best, cur, start = (0, 0), 0, 0
        for i, m in enumerate(msk):
            if m:
                cur = 0
            else:
                if cur == 0:
                    start = i
                cur += 1
                if cur > best[1]:
                    best = (start, cur)
        s, ln = best
        if ln < 4:
            return []
        anchor = pat[s:s + ln]
        res = []
        i = code.find(anchor, max(0, lo + s))
        while 0 <= i and i - s < hi:
            p = i - s
            if p >= lo and p + len(pat) <= len(code):
                if all(msk[k] or bm[p + k] or code[p + k] == pat[k] for k in range(len(pat))):
                    res.append(p)
            i = code.find(anchor, i + 1)
        return res

    def _bracket(self, addr):
        i = bisect.bisect_right(self.anchor_old, addr) - 1
        lo_old, lo_new = self.anchors[i] if i >= 0 else (self.a.code_base, self.b.code_base)
        hi_new = self.anchors[i + 1][1] if i + 1 < len(self.anchors) else self.b.code_base + len(self.b.code)
        return lo_old, lo_new, hi_new

    def same(self, a, n, length=16):
        oa, ob = a - self.a.code_base, n - self.b.code_base
        if oa < 0 or ob < 0 or oa + length > len(self.a.code) or ob + length > len(self.b.code):
            return False
        return all(self.a.mask[oa + k] or self.b.mask[ob + k] or self.a.code[oa + k] == self.b.code[ob + k]
                   for k in range(length))

    def code(self, addr):
        """new address of old code address, or (None, reason)"""
        lo_old, lo_new, hi_new = self._bracket(addr)
        lo, hi = lo_new - self.b.code_base, hi_new - self.b.code_base + 256
        if addr == lo_old:
            return lo_new, 'anchor'
        for n in (24, 40, 64, 96, 160):
            w = self.a.window(addr, n)
            if not w:
                break
            hits = self._find(*w, lo=lo, hi=hi)
            if len(hits) == 1:
                return self.b.code_base + hits[0], 'fwd%d' % n
            if not hits:
                break
        for back in (16, 32, 64, 128, 256):
            for n in (back + 24, back + 64):
                w = self.a.window(addr - back, n)
                if not w:
                    continue
                hits = self._find(*w, lo=lo - back, hi=hi)
                if len(hits) == 1:
                    return self.b.code_base + hits[0] + back, 'back%d/%d' % (back, n)
        n = addr + (lo_new - lo_old)
        if self.same(addr, n, 24):
            return n, 'anchor shift %+x' % (lo_new - lo_old)
        return None, 'no unique match'

    def data(self, addr, max_below=0x40):
        """new address of old data address via the code that references it"""
        i = bisect.bisect_right(self.a.targets, addr) - 1
        votes = collections.Counter()
        while i >= 0 and addr - self.a.targets[i] <= max_below:
            t = self.a.targets[i]
            for s in self.a.by_target[t]:
                if not (self.a.code_base <= s < self.a.code_base + len(self.a.code)):
                    continue
                ns, _ = self.code(s)
                if ns is None:
                    continue
                nt = self.b.fix.get(ns)
                if nt is not None:
                    votes[nt + (addr - t)] += 1
            if votes:
                break
            i -= 1
        if not votes:
            return None, 'no references'
        (v, n), = votes.most_common(1)
        return v, 'data %d/%d votes' % (n, sum(votes.values()))


if __name__ == '__main__':
    m = Mapper(sys.argv[1], sys.argv[2])
    print('%d anchors (%d dropped as out of order)' % (len(m.anchors), m.dropped), file=sys.stderr)
    for s in sys.argv[3:]:
        a = int(s, 16)
        r, how = (m.code(a) if a < m.a.data_base else m.data(a))
        print('%x %s %s' % (a, '%x' % r if r is not None else '-', how))
