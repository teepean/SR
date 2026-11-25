# images.py - image instructions of a guest program, translated against the images and samplers the emulator (Kyty)
# binds for the program's specialisation.
#
# Kyty's dump (KYTY_TRANSLATED_DUMP, <hash>.<key>.res) lists, per specialisation, every image of the program: the
# guest PC of its first use, its class (sampled / storage), numeric class, dimension, depth compare, and where Kyty
# binds it: set 0, binding = descriptor kind, array element = descriptor_index; samplers: binding 51, element = the
# sampler's index. The translated pipeline uses Kyty's layout and Kyty commits the descriptors, so the translation
# declares the same bindings and indexes the same elements.
#
# Matching an instruction to Kyty's image: Kyty merges image uses by descriptor source (+ class, dimension, mip mode,
# depth compare). The first use of every Kyty image is one of our instructions; another instruction uses the same image
# when the registers holding its descriptor have the same defining instructions (program order) and it has the same
# dimension and compare mode (and the class its operation family has in this program).
import collections
import re

CLASS_SAMPLED, CLASS_STORAGE = 1, 2
NUMERIC_FLOAT, NUMERIC_UINT, NUMERIC_SINT = 1, 2, 3
# Kyty's Decoder::ImageDimension
DIM_1D, DIM_1D_ARRAY, DIM_2D, DIM_3D, DIM_2D_ARRAY, DIM_2D_MSAA, DIM_2D_MSAA_ARRAY = 1, 2, 3, 4, 5, 6, 7
# as Kyty declares them (spirvEmitterInternal.h ImageDimensions): 1D images are 2D views of one row
DIM_SUFFIX = {DIM_1D: '2D', DIM_1D_ARRAY: '2DArray', DIM_2D: '2D', DIM_3D: '3D', DIM_2D_ARRAY: '2DArray',
              DIM_2D_MSAA: '2DMS', DIM_2D_MSAA_ARRAY: '2DMSArray'}
ONE_DIMENSIONAL = (DIM_1D, DIM_1D_ARRAY)
# coordinates of the image as bound (Kyty takes the dimension from the T#, which may differ from the instruction's)
KYTY_COORDS = {DIM_1D: 1, DIM_1D_ARRAY: 2, DIM_2D: 2, DIM_3D: 3, DIM_2D_ARRAY: 3, DIM_2D_MSAA: 2, DIM_2D_MSAA_ARRAY: 3}
# guest coordinates of the image as bound; a one-dimensional image gets a row coordinate after x
# guest dim modifier -> (address components of the instruction, layer component)
GUEST_DIM = {
    'SQ_RSRC_IMG_1D': (1, False), 'SQ_RSRC_IMG_1D_ARRAY': (2, True), 'SQ_RSRC_IMG_2D': (2, False),
    'SQ_RSRC_IMG_2D_ARRAY': (3, True), 'SQ_RSRC_IMG_3D': (3, False), 'SQ_RSRC_IMG_CUBE': (3, True),
    'SQ_RSRC_IMG_2D_MSAA': (3, False), 'SQ_RSRC_IMG_2D_MSAA_ARRAY': (4, True),
}


class Unsupported(Exception):
    pass


def load_resources(path):
    """the .res file: {'images': [dict], 'samplers': [dict], 'pairs': [(image, sampler)], 'key': str}"""
    res = {'images': [], 'samplers': [], 'pairs': [], 'key': None}
    for line in open(path):
        words = line.split()
        if not words:
            continue
        if words[0] == 'hash':
            res['key'] = words[3]
            continue
        if words[0] in ('image', 'sampler'):
            d = {'index': int(words[1])}
            for k in range(2, len(words) - 1, 2):
                v = words[k + 1]
                d[words[k]] = int(v, 0) if re.match(r'^-?(0x[0-9a-fA-F]+|\d+)$', v) else v
            res[words[0] + 's'].append(d)
        elif words[0] == 'pair':
            res['pairs'].append((int(words[2]), int(words[4])))
    return res


def load_resources_gres(path):
    """a graphics module record (.gres, Kyty's graphics plug-in): the same image / sampler / pair lines as a .res, with
    the stage's Vulkan binding of each image (vk_binding; graphics stages offset Kyty's descriptor kinds) and the
    stage's sampler binding (the 'binding ... kind Samplers' line)"""
    res = {'images': [], 'samplers': [], 'pairs': [], 'key': None, 'sampler_binding': None}
    for line in open(path):
        words = line.split()
        if not words:
            continue
        if words[0] == 'binding' and 'kind' in words:
            d = dict(zip(words[1::2], words[2::2]))
            if d.get('kind') == 'Samplers':
                res['sampler_binding'] = int(d['binding'])
        elif words[0] in ('image', 'sampler'):
            d = {'index': int(words[1])}
            for k in range(2, len(words) - 1, 2):
                v = words[k + 1]
                d[words[k]] = int(v, 0) if re.match(r'^-?(0x[0-9a-fA-F]+|\d+)$', v) else v
            if words[0] == 'image' and 'vk_binding' in d:
                d['kind_binding'] = d['binding']
                d['binding'] = d['vk_binding']
            res[words[0] + 's'].append(d)
        elif words[0] == 'pair':
            res['pairs'].append((int(words[2]), int(words[4])))
    return res


def family(base):
    if base.startswith('image_sample') or base.startswith('image_gather'):
        return 'sample'
    if base.startswith('image_store'):
        return 'store'
    if base.startswith('image_atomic'):
        return 'atomic'
    if base.startswith('image_load'):
        return 'load'
    return 'other'


def operands(inst):
    """(vdata, vaddr, T#, S# or None)"""
    ops = inst.ops
    if family(inst.base) == 'sample':
        return ops[0], ops[1], ops[2], ops[3]
    return ops[0], ops[1], ops[2], None


class ImageMap:
    """instruction offset -> (Kyty image dict, Kyty sampler dict or None)"""

    def __init__(self, insts, res, all_sgpr_defs, user_sgprs=16):
        self.res = res
        # value numbers of the scalar registers (program order): user data, loads (pointer value numbers + offset),
        # copies; any other write is a value of its own. Kyty merges image uses by the source of the descriptor dwords,
        # which these numbers follow: a descriptor loaded again from the same place has the same numbers
        vn = {r: ('user', r) for r in range(user_sgprs)}
        tkey, skey = {}, {}
        for i in insts:
            if i.base.startswith('image_'):
                _, _, t, s = operands(i)
                tkey[i.off] = tuple(vn.get(t.first + k, ('undef', t.first + k)) for k in range(t.count))
                skey[i.off] = (tuple(vn.get(s.first + k, ('undef', s.first + k)) for k in range(s.count))
                               if s is not None else None)
            defs = [int(r[1:]) for r in all_sgpr_defs(i)]
            new = {}
            if i.base.startswith(('s_load_dword', 's_buffer_load_dword')) and i.ops and i.ops[0].kind == 'reg':
                d, base, off = i.ops[0], i.ops[1], i.ops[2]
                imm = int(i.mods.get('offset', '0'), 0)
                if off.kind == 'const':
                    o = ('imm', off.value + imm)
                elif off.kind == 'null':
                    o = ('imm', imm)
                else:
                    o = ('reg', vn.get(off.first, ('undef', off.first)), imm)
                src = tuple(vn.get(base.first + k, ('undef', base.first + k)) for k in range(base.count))
                for k in range(d.count):
                    new[d.first + k] = ('load', i.base.startswith('s_buffer'), src, o, 4 * k)
            elif i.base in ('s_mov_b32', 's_mov_b64') and i.ops[0].kind == 'reg' and i.ops[1].kind == 'reg':
                for k in range(i.ops[0].count):
                    new[i.ops[0].first + k] = vn.get(i.ops[1].first + k, ('undef', i.ops[1].first + k))
            for r in defs:
                vn[r] = new.get(r, ('def', i.off, r))
        self.insts = {i.off: i for i in insts}
        by_pc = {i.off: i for i in insts if i.base.startswith('image_')}
        # anchors: the first use of each Kyty image / sampler is one of our instructions
        anchors = collections.defaultdict(list)       # (tkey, dim, depth) -> [image]
        family_class = {}
        for img in res['images']:
            if img.get('heap') or img.get('table_capacity'):
                raise Unsupported('image %d comes from a descriptor table / the image heap' % img['index'])
            a = by_pc.get(img['pc'])
            if a is None:
                raise Unsupported('first use of image %d (pc %#x) is not an image instruction' % (img['index'], img['pc']))
            anchors[self.image_key(a, tkey)].append(img)
            family_class.setdefault(family(a.base), img['class'])
        sampler_anchors = collections.defaultdict(list)
        for smp in res['samplers']:
            if smp.get('indirect_root', -1) not in (-1, 0xffffffff):
                raise Unsupported('sampler %d is indexed' % smp['index'])
            a = by_pc.get(smp['pc'])
            if a is None or skey.get(a.off) is None:
                raise Unsupported('first use of sampler %d (pc %#x) is not a sampling instruction' % (
                    smp['index'], smp['pc']))
            sampler_anchors[skey[a.off]].append(smp)
        self.map = {}
        for off, i in by_pc.items():
            cands = anchors.get(self.image_key(i, tkey), [])
            fam = family(i.base)
            want = family_class.get(fam, CLASS_SAMPLED if fam == 'sample' else CLASS_STORAGE)
            cands = [c for c in cands if c['class'] == want] if len(cands) > 1 else cands
            if len(cands) != 1:
                raise Unsupported('%s: %d matching images' % (i, len(cands)))
            smp = None
            if fam == 'sample':
                sc = sampler_anchors.get(skey[off], [])
                if len(sc) != 1:
                    raise Unsupported('%s: %d matching samplers' % (i, len(sc)))
                smp = sc[0]
            self.map[off] = (cands[0], smp)

    @staticmethod
    def image_key(inst, tkey):
        depth = inst.base.startswith(('image_sample_c', 'image_gather4_c'))
        return tkey[inst.off], inst.mods.get('dim', ''), depth

    # ------------------------------------------------------------------------------------------- declarations
    def declarations(self):
        """GLSL declarations of the bindings the program's images use (Kyty's layout: set 0)"""
        by_binding = {}
        for img, _ in self.map.values():
            b = img['binding']
            if b < 0:
                raise Unsupported('image %d has no binding' % img['index'])
            n = img['element'] + max(img.get('mips', 1), 1)
            t = glsl_type(img)
            old = by_binding.get(b)
            if old is not None and old[0] != t:
                raise Unsupported('binding %d holds %s and %s' % (b, old[0], t))
            by_binding[b] = (t, max(n, old[1] if old else 0))
        lines = []
        for b, (t, n) in sorted(by_binding.items()):
            lines.append('layout(set = 0, binding = %d) uniform %s IMG%d[%d];' % (b, t, b, n))
        nsmp = max((s['index'] + 1 for _, s in self.map.values() if s is not None), default=0)
        if nsmp:
            sb = self.res.get('sampler_binding')
            lines.append('layout(set = 0, binding = %d) uniform sampler SMP[%d];' % (51 if sb is None else sb, nsmp))
        return '\n'.join(lines)


def glsl_type(img):
    dim = DIM_SUFFIX.get(img['dim'])
    if dim is None:
        raise Unsupported('image %d: dimension %d' % (img['index'], img['dim']))
    prefix = {NUMERIC_FLOAT: '', NUMERIC_UINT: 'u', NUMERIC_SINT: 'i'}.get(img['numeric'])
    if prefix is None:
        raise Unsupported('image %d: numeric class %d' % (img['index'], img['numeric']))
    if img['class'] == CLASS_SAMPLED:
        return '%stexture%s' % (prefix, dim)
    if img['class'] == CLASS_STORAGE:
        if img.get('atomic'):
            raise Unsupported('image %d: atomics' % img['index'])
        return '%simage%s' % (prefix, dim)
    raise Unsupported('image %d: class %d' % (img['index'], img['class']))
