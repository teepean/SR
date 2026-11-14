#!/usr/bin/env python3
"""HD texture packs for the recompiled AvP2 / Primal Hunt (render3d.cpp's hd_textures setting).

  hdtex.py export GAMEDIR... OUTDIR every DTX texture in the GAMEDIRs' REZ files -> OUTDIR/<its path>.png (mip 0,
                                    RGBA) + OUTDIR/manifest.txt. Primal Hunt also uses AvP2's REZ files: give both
                                    directories (one pack serves both games: the textures are found by content)
  hdtex.py check OUTDIR             the manifest's images: present, readable, the original's aspect ratio

Workflow: export from your own copy of the game, upscale / edit the PNGs (keep their names; another format with the
same name works too: .dds with mipmaps (BC1-7), .webp, .jpg, .tga), then point SR-AvP2.cfg at the directory:
hd_textures = /path/to/OUTDIR. The game replaces each texture whose original content is in the manifest; anything
else (a missing or unreadable image, another aspect ratio) keeps the original. Images without alpha get the
original's alpha (upscaled) where the original has one. The textures come from the game: a pack made from them is
for your own use, not to be redistributed.

The manifest maps the MD5 (first 8 bytes, hex) of a texture's mip data as the engine holds it - DXT blocks, or
32-bit BGRA rows - to an image, for every mip level of the file (the engine's first mip is the file's first one;
the others in case a setting drops levels). Tab-separated: hash, image (relative), original width x height, format.
Textures with the same content under several paths share one image.

Needs Pillow (DXT decoding, PNG writing)."""
import hashlib, io, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rez

HEADER = 164
FORMATS = {3: 'bgra32', 4: 'dxt1', 5: 'dxt3', 6: 'dxt5'}


def mip_size(bpp, w, h):
    if bpp in (4, 5, 6):
        return ((w + 3) // 4) * ((h + 3) // 4) * (8 if bpp == 4 else 16)
    if bpp == 3:
        return w * h * 4
    return None


def parse_dtx(d, p, s):
    """(width, height, bpp, [(mip w, mip h, mip bytes)]) or None (formats the game's renderer doesn't take)"""
    if s < HEADER:
        return None
    rt, ver, w, h, nm, ns, ifl, ufl = struct.unpack_from('<IiHHHHii', d, p)
    if ver != -5 or w == 0 or h == 0:
        return None
    bpp = d[p + 24 + 2] or 3      # DtxHeader::GetBPPIdent: 0 = 32-bit
    if bpp not in FORMATS:
        return None
    mips, o = [], p + HEADER
    for m in range(max(nm, 1)):
        mw, mh = max(w >> m, 1), max(h >> m, 1)
        n = mip_size(bpp, mw, mh)
        if o + n > p + s:
            break
        mips.append((mw, mh, d[o:o + n]))
        o += n
    return (w, h, bpp, mips) if mips else None


def mip_hash(data):
    return hashlib.md5(data).hexdigest()[:16]


def decode(bpp, w, h, data):
    from PIL import Image
    if bpp == 3:
        return Image.frombytes('RGBA', (w, h), data, 'raw', 'BGRA')
    # a DDS around the blocks: Pillow decodes DXT1/3/5
    fourcc = {4: b'DXT1', 5: b'DXT3', 6: b'DXT5'}[bpp]
    hdr = struct.pack('<4s7I44x8I5I', b'DDS ', 124, 0x81007, h, w, len(data), 0, 0,
                      32, 4, struct.unpack('<I', fourcc)[0], 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
    img = Image.open(io.BytesIO(hdr + data))
    img.load()
    return img.convert('RGBA')


def rez_files(gamedir):
    return sorted(os.path.join(gamedir, f) for f in os.listdir(gamedir) if f.lower().endswith('.rez'))


def export(gamedirs, outdir):
    os.makedirs(outdir, exist_ok=True)
    manifest = {}           # hash -> (image, w, h, format)
    images = {}             # image path (lower case) -> content hash
    count = shared = 0
    for fn in [f for g in gamedirs for f in rez_files(g)]:
        d, ents = rez.entries(fn)
        for name, p, s in ents:
            if not name.lower().endswith('.dtx'):
                continue
            t = parse_dtx(d, p, s)
            if t is None:
                continue
            w, h, bpp, mips = t
            key = mip_hash(mips[0][2])
            if key in manifest:
                shared += 1
                continue
            rel = name[:-4] + '.png'
            if rel.lower() in images and images[rel.lower()] != key:
                rel = name[:-4] + '.' + key[:8] + '.png'
            images[rel.lower()] = key
            dst = os.path.join(outdir, *rel.split('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            decode(bpp, mips[0][0], mips[0][1], mips[0][2]).save(dst)
            for mw, mh, md in mips:
                manifest.setdefault(mip_hash(md), (rel, w, h, FORMATS[bpp]))
            count += 1
        print('%s: done (%d textures so far)' % (os.path.basename(fn), count), file=sys.stderr)
    with open(os.path.join(outdir, 'manifest.txt'), 'w', newline='\n') as f:
        f.write('# hdtex manifest 1: MD5[0:8] of the mip data as the engine holds it, image, original size, format\n')
        for k in sorted(manifest):
            rel, w, h, fmt = manifest[k]
            f.write('%s\t%s\t%dx%d\t%s\n' % (k, rel, w, h, fmt))
    print('%d textures exported (%d more with the same content), %d hashes -> %s' % (count, shared, len(manifest), outdir))
    return 0


def check(outdir):
    from PIL import Image
    bad = n = 0
    seen = set()
    for line in open(os.path.join(outdir, 'manifest.txt'), encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        k, rel, size, fmt = line.rstrip('\n').split('\t')
        if rel in seen:
            continue
        seen.add(rel)
        n += 1
        w, h = map(int, size.split('x'))
        path = os.path.join(outdir, *rel.split('/'))
        stem = os.path.splitext(path)[0]
        found = [path] if os.path.exists(path) else [stem + e for e in ('.dds', '.webp', '.jpg', '.tga') if os.path.exists(stem + e)]
        if not found:
            print('missing: %s' % rel); bad += 1; continue
        try:
            with Image.open(found[0]) as im:
                iw, ih = im.size
        except Exception as e:
            print('unreadable: %s (%s)' % (found[0], e)); bad += 1; continue
        if iw * h != ih * w:
            print('aspect: %s is %dx%d, the original %dx%d' % (rel, iw, ih, w, h)); bad += 1
    print('%d images, %d problems' % (n, bad))
    return 1 if bad else 0


def main(a):
    if len(a) >= 4 and a[1] == 'export':
        return export(a[2:-1], a[-1])
    if len(a) == 3 and a[1] == 'check':
        return check(a[2])
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
