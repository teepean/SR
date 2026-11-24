#! /usr/bin/env python3
# srmap.py - maps addresses of an SRW64 build back to the original program using its loc_<original address> symbols
# usage as a script: srmap.py <recompiled executable> <address>... (prints anchor, original address and distance)
# as a module: m = SRMap(path); m.original(address) -> (original anchor address, bytes after the anchor) or None
import bisect, subprocess, sys

class SRMap:
    def __init__(self, path):
        pairs = []
        out = subprocess.run(['nm', path], capture_output=True, text=True).stdout
        for line in out.split('\n'):
            p = line.split()
            if len(p) == 3 and p[1] in 'tT' and p[2].startswith('loc_'):
                try: pairs.append((int(p[0], 16), int(p[2][4:], 16)))
                except ValueError: pass
        pairs.sort()
        self.new = [a for a, _ in pairs]
        self.orig = [o for _, o in pairs]

    def original(self, address):
        i = bisect.bisect_right(self.new, address) - 1
        if i < 0: return None
        return self.orig[i], address - self.new[i]

if __name__ == '__main__':
    m = SRMap(sys.argv[1])
    for text in sys.argv[2:]:
        r = m.original(int(text, 0))
        print('%s -> %s' % (text, 'loc_%X + 0x%x' % r if r else '?'))
