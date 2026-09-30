#!/usr/bin/env python3
"""
Interstate '76 archive extractor (I76.ZFS) for SR-I76.

  i76extract.py list     [--zfs I76.ZFS] [pattern ...]
  i76extract.py extract  [--zfs I76.ZFS] OUTDIR [pattern ...]
  i76extract.py textures [--zfs I76.ZFS] OUTDIR [pattern ...]

list      lists the archive's files and the members of its .pak files (pak:member)
extract   writes the files (decompressed) to OUTDIR; the members of x.pak go to OUTDIR/x/<member>
textures  converts the 3D textures (.m16) to PNG: OUTDIR/<pak>/<NAME>.png, and writes
          OUTDIR/texpack.txt, which maps the game's texture names (<w>x<h>_<hash>, see texpack.c)
          to the PNG files. A copy of OUTDIR works as SR-I76's texture pack (texture_pack):
          edit or upscale the images you want to replace and delete the others.

Patterns are shell-style (fnmatch, case-insensitive), matched against the file name and, for
pak members, also against "pak:member" (e.g. "zdash*", "vfcoupe6.pak:*").

Formats: ZFS = header + directory blocks (16-byte names), files stored raw or compressed with
LZO1X (2) / LZO1Y (4). pix = text index (count, then name offset length) of the matching pak.
m16 = u32 width, u24 height + u8 flags, width*height palette indices, u32 count, count RGB565
colors; index 255 is transparent when the game draws the texture with the chroma key.
"""

import fnmatch
import os
import struct
import sys
import zlib


# ---------------------------------------------------------------- LZO

def lzo_decompress(src, out_len, method):
    """LZO1X (method 2) / LZO1Y (method 4) decompression (after lzo1x_d.ch)."""
    if method == 2:
        m2_max, m2_shift, m2_mask = 0x800, 3, 7
    else:
        m2_max, m2_shift, m2_mask = 0x400, 2, 3
    out = bytearray()
    ip = 0

    def copy_match(pos, length):
        if pos < 0:
            raise ValueError('lzo: bad match offset')
        if len(out) - pos >= length:
            out.extend(out[pos:pos + length])
        else:
            for k in range(length):
                out.append(out[pos + k])

    t = src[0]
    if t > 17:
        ip = 1
        t -= 17
        out += src[ip:ip + t]
        ip += t
        state = 'after_literal' if t >= 4 else 'match_next'
    else:
        state = 'loop'

    while True:
        if state == 'loop':
            t = src[ip]; ip += 1
            if t < 16:
                if t == 0:
                    while src[ip] == 0:
                        t += 255; ip += 1
                    t += 15 + src[ip]; ip += 1
                t += 3
                out += src[ip:ip + t]; ip += t
                state = 'after_literal'
                continue
        elif state == 'after_literal':
            t = src[ip]; ip += 1
            if t < 16:
                copy_match(len(out) - (1 + m2_max) - (t >> 2) - (src[ip] << 2), 3); ip += 1
                t = src[ip - 2] & 3
                if t == 0:
                    state = 'loop'
                    continue
                out += src[ip:ip + t]; ip += t
                t = src[ip]; ip += 1
        else:  # match_next: t = literal count (1..3) after the first literal run
            out += src[ip:ip + t]; ip += t
            t = src[ip]; ip += 1

        # matches; t = opcode
        while True:
            if t >= 64:
                pos = len(out) - 1 - ((t >> 2) & m2_mask) - (src[ip] << m2_shift); ip += 1
                length = ((t >> 5) - 1 if method == 2 else (t >> 4) - 3) + 2
            elif t >= 32:
                length = t & 31
                if length == 0:
                    while src[ip] == 0:
                        length += 255; ip += 1
                    length += 31 + src[ip]; ip += 1
                length += 2
                pos = len(out) - 1 - ((src[ip] | (src[ip + 1] << 8)) >> 2); ip += 2
            elif t >= 16:
                pos = len(out) - ((t & 8) << 11)
                length = t & 7
                if length == 0:
                    while src[ip] == 0:
                        length += 255; ip += 1
                    length += 7 + src[ip]; ip += 1
                length += 2
                pos -= (src[ip] | (src[ip + 1] << 8)) >> 2; ip += 2
                if pos == len(out):
                    # end of stream
                    if len(out) != out_len:
                        raise ValueError('lzo: %d bytes instead of %d' % (len(out), out_len))
                    return bytes(out)
                pos -= 0x4000
            else:
                pos = len(out) - 1 - (t >> 2) - (src[ip] << 2); ip += 1
                length = 2
            copy_match(pos, length)
            t = src[ip - 2] & 3
            if t == 0:
                break
            out += src[ip:ip + t]; ip += t
            t = src[ip]; ip += 1
        state = 'loop'


# ---------------------------------------------------------------- archives

class ZFS:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        magic, _, _, per_dir, total, _, _ = struct.unpack_from('<4s6I', d, 0)
        if magic != b'ZFSF':
            raise ValueError('%s: not a ZFS archive' % path)
        self.files = {}     # lower-case name -> (name, offset, length, method, size)
        self.order = []
        off = 28
        while off and len(self.order) < total:
            nxt = struct.unpack_from('<I', d, off)[0]
            p = off + 4
            for _ in range(per_dir):
                name, data_off, _, length, _, flags = struct.unpack_from('<16sIIIII', d, p)
                p += 36
                name = name.split(b'\0')[0].decode('latin-1')
                if name:
                    self.files[name.lower()] = (name, data_off, length, flags & 0xFF, flags >> 8)
                    self.order.append(name.lower())
            off = nxt

    def name(self, key):
        return self.files[key.lower()][0]

    def read(self, key):
        _, off, length, method, size = self.files[key.lower()]
        raw = self.data[off:off + length]
        if method == 0:
            return raw
        if method in (2, 4):
            return lzo_decompress(raw, size, method)
        raise ValueError('%s: unknown compression %d' % (key, method))

    def pak_members(self, pix_key):
        """Members of the pak described by a .pix file: [(name, offset, length)]."""
        tokens = self.read(pix_key).decode('latin-1').split()
        count = int(tokens[0])
        return [(tokens[1 + 3 * i], int(tokens[2 + 3 * i]), int(tokens[3 + 3 * i])) for i in range(count)]

    def entries(self):
        """All files and pak members: (display name, pak key or None, member name or key, reader)."""
        for key in self.order:
            name = self.files[key][0]
            yield (name, None, name, (lambda k=key: self.read(k)))
            if key.endswith('.pix'):
                pak = key[:-4] + '.pak'
                if pak not in self.files:
                    continue
                cache = {}

                def read_member(off, length, pak=pak, cache=cache):
                    if 'd' not in cache:
                        cache['d'] = self.read(pak)
                    return cache['d'][off:off + length]
                pak_name = self.files[pak][0]
                for mname, off, length in self.pak_members(key):
                    yield ('%s:%s' % (pak_name, mname), pak_name, mname,
                           (lambda o=off, l=length, r=read_member: r(o, l)))


def matches(entry_name, member, patterns):
    if not patterns:
        return True
    names = (entry_name.lower(), member.lower())
    return any(fnmatch.fnmatchcase(n, p.lower()) for p in patterns for n in names)


# ---------------------------------------------------------------- textures

def fnv1a64(w, h, rgba):
    """texpack.c hash_pixels: FNV-1a 64 over the u32 width, u32 height and the RGBA bytes."""
    x = 0xcbf29ce484222325
    for c in struct.pack('<II', w, h):
        x = ((x ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    for c in rgba:
        x = ((x ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return x if x != 0 else 1


def expand5(v):
    return (v << 3) | (v >> 2)


def decode_m16(data):
    """Returns (w, h, rgba_chroma, rgba_opaque, has_key): the texture as the game's texture pack sees it
    (ZGLIDE converts the RGB565 palette to ARGB1555; index 255 becomes transparent black when the game
    uses the chroma key), and the PNG pixels."""
    w, h = struct.unpack_from('<II', data, 0)
    h &= 0xFFFFFF
    pixels = data[8:8 + w * h]
    count = struct.unpack_from('<I', data, 8 + w * h)[0]
    palette = struct.unpack_from('<%dH' % count, data, 12 + w * h)
    lut = []
    for i in range(256):
        v = palette[i] if i < count else 0
        # RGB565 -> 8 bits per channel (ZGLIDE) -> ARGB1555 (top 5 bits) -> 8 bits (glide.c)
        lut.append(bytes((expand5((v >> 11) & 31), expand5((v >> 6) & 31), expand5(v & 31), 255)))
    opaque = b''.join([lut[p] for p in pixels])
    has_key = 255 in pixels
    if has_key:
        keyed = list(lut)
        keyed[255] = b'\0\0\0\0'
        chroma = b''.join([keyed[p] for p in pixels])
    else:
        chroma = opaque
    return w, h, chroma, opaque, has_key


def write_png(path, w, h, rgba):
    rows = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, payload):
        return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(rows, 9)))
        f.write(chunk(b'IEND', b''))


def safe_part(name):
    return ''.join(c if (c.isalnum() or c in '._-') else '_' for c in name)


# ---------------------------------------------------------------- commands

def cmd_list(z, patterns):
    for name, pak, member, read in z.entries():
        if matches(name, member, patterns):
            print(name)


def cmd_extract(z, outdir, patterns):
    count = 0
    for name, pak, member, read in z.entries():
        if not matches(name, member, patterns):
            continue
        d = os.path.join(outdir, os.path.splitext(safe_part(pak))[0]) if pak else outdir
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, safe_part(member)), 'wb') as f:
            f.write(read())
        count += 1
    print('%d files written to %s' % (count, outdir))


def cmd_textures(z, outdir, patterns):
    os.makedirs(outdir, exist_ok=True)
    seen = {}       # hash name -> PNG path (relative to outdir)
    index = []
    written = dupes = 0
    for name, pak, member, read in z.entries():
        if not member.lower().endswith('.m16') or not matches(name, member, patterns):
            continue
        try:
            w, h, chroma, opaque, has_key = decode_m16(read())
        except (struct.error, ValueError) as e:
            print('%s: %s' % (name, e), file=sys.stderr)
            continue
        keys = ['%dx%d_%016x' % (w, h, fnv1a64(w, h, chroma))]
        if has_key:
            keys.append('%dx%d_%016x' % (w, h, fnv1a64(w, h, opaque)))
        if keys[0] in seen:
            dupes += 1
            continue
        rel_dir = os.path.splitext(safe_part(pak))[0] if pak else '.'
        rel = os.path.normpath(os.path.join(rel_dir, os.path.splitext(safe_part(member))[0] + '.png'))
        os.makedirs(os.path.join(outdir, rel_dir), exist_ok=True)
        write_png(os.path.join(outdir, rel), w, h, chroma)
        written += 1
        for k in keys:
            if k not in seen:
                seen[k] = rel
                index.append('%s %s' % (k, rel.replace(os.sep, '/')))
        if written % 500 == 0:
            print('%d textures...' % written)
    with open(os.path.join(outdir, 'texpack.txt'), 'w', newline='\n') as f:
        f.write('# SR-I76 texture pack index: <w>x<h>_<hash> <image file>\n')
        f.write('\n'.join(index) + '\n')
    print('%d textures written to %s (%d duplicates skipped), index %s' %
          (written, outdir, dupes, os.path.join(outdir, 'texpack.txt')))


def main(argv):
    args = list(argv[1:])
    zfs_path = None
    if '--zfs' in args:
        i = args.index('--zfs')
        zfs_path = args[i + 1]
        del args[i:i + 2]
    if not args or args[0] not in ('list', 'extract', 'textures') or (args[0] != 'list' and len(args) < 2):
        print(__doc__.strip())
        return 1
    if zfs_path is None:
        zfs_path = next((n for n in os.listdir('.') if n.lower() == 'i76.zfs'), 'I76.ZFS')
    z = ZFS(zfs_path)
    if args[0] == 'list':
        cmd_list(z, args[1:])
    elif args[0] == 'extract':
        cmd_extract(z, args[1], args[2:])
    else:
        cmd_textures(z, args[1], args[2:])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
