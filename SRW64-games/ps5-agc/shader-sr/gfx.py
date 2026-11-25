# gfx.py - the interface of a graphics module as Kyty compiles it (KYTY_GRAPHICS_DUMP records), for gcn2glsl's
# graphics stages: Kyty's plug-in (pipeline/graphicsTranslated.h) swaps one Kyty module (stage, hash, module id) for a
# translation that declares a subset of the module's interface, so a translation is generated per module id from the
# module's record (<STAGE>_<hash>_<id>.gres) and Kyty's SPIR-V of it (<STAGE>_<hash>_<id>.spv, for the builtins).

import collections
import struct


class Interface:
    def __init__(self):
        self.stage = None
        self.hash = None
        self.id = None
        self.user_data_base = 0
        self.outputs = []          # (kind, index, location) of the module's outputs
        self.inputs = []           # dicts of the input lines
        self.builtins_out = set()  # output builtins of Kyty's module (SPIR-V BuiltIn decorations)
        self.builtins_in = set()
        self.indirect_draw = False
        self.indirect_offsets = (255, 255)
        self.pixel = {}            # the 'pixel' line (key -> value)
        self.target_output_mode = []
        self.interpolators = []
        self.images = []           # the image lines (key -> value)
        self.samplers = []
        self.bindings = []

    def param_location(self, n):
        """the output location of the guest's parameter export n (exp param<n>), or None"""
        for kind, index, location in self.outputs:
            if kind == 'Parameter' and index == n:
                return location
        return None


def _kv(words):
    d = {}
    for k in range(0, len(words) - 1, 2):
        d[words[k]] = words[k + 1]
    return d


# SPIR-V: BuiltIn decoration values that matter here
BUILTINS = {0: 'Position', 1: 'PointSize', 3: 'ClipDistance', 4: 'CullDistance', 15: 'FragCoord', 17: 'FrontFacing',
            22: 'FragDepth', 23: 'HelperInvocation', 42: 'VertexIndex', 43: 'InstanceIndex', 4424: 'BaseVertex',
            4425: 'BaseInstance', 20: 'SampleMask', 18: 'SampleId', 19: 'SamplePosition', 7: 'PrimitiveId', 9: 'Layer',
            10: 'ViewportIndex'}


def spirv_builtins(path):
    """(input builtins, output builtins) of a SPIR-V module's variables (OpDecorate / OpMemberDecorate BuiltIn)"""
    data = open(path, 'rb').read()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    decorated = {}             # id -> builtin name (variables), or struct id -> set of member builtins
    storage = {}               # variable id -> storage class
    pointee = {}               # pointer type id -> pointee type id
    var_type = {}
    k = 5
    while k < len(words):
        count, op = words[k] >> 16, words[k] & 0xffff
        ops = words[k + 1:k + count]
        if op == 71 and len(ops) >= 3 and ops[1] == 11:            # OpDecorate %id BuiltIn b
            decorated[ops[0]] = BUILTINS.get(ops[2], str(ops[2]))
        elif op == 72 and len(ops) >= 4 and ops[2] == 11:          # OpMemberDecorate %struct m BuiltIn b
            decorated.setdefault(('struct', ops[0]), set()).add(BUILTINS.get(ops[3], str(ops[3])))
        elif op == 32 and len(ops) >= 3:                            # OpTypePointer %r storage %type
            pointee[ops[0]] = ops[2]
        elif op == 59 and len(ops) >= 3:                            # OpVariable %type %r storage
            storage[ops[1]] = ops[2]
            var_type[ops[1]] = ops[0]
        if count == 0:
            break
        k += count
    ins, outs = set(), set()
    for var, sc in storage.items():
        names = set()
        if var in decorated:
            names.add(decorated[var])
        t = pointee.get(var_type.get(var))
        if ('struct', t) in decorated:
            names |= decorated[('struct', t)]
        if sc == 1:
            ins |= names
        elif sc == 3:
            outs |= names
    return ins, outs


# SPIR-V decorations of interface variables
DEC_NAMES = {13: 'NoPerspective', 14: 'Flat', 16: 'Centroid', 17: 'Sample', 5285: 'PerVertex'}


def spirv_interface(path):
    """the module's Input / Output variables with a Location: {('in'|'out', location): {'type': 'float'|'uint'|'int',
    'n': components, 'array': length or None, 'decorations': set, 'index': Index decoration or None}}"""
    data = open(path, 'rb').read()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    types, consts, loc, index, decs, var = {}, {}, {}, {}, collections.defaultdict(set), {}
    k = 5
    while k < len(words):
        count, op = words[k] >> 16, words[k] & 0xffff
        o = words[k + 1:k + count]
        if op == 21:
            types[o[0]] = ('int' if o[2] else 'uint', 1, None)
        elif op == 22:
            types[o[0]] = ('float', 1, None)
        elif op == 20:
            types[o[0]] = ('bool', 1, None)
        elif op == 23 and o[1] in types:
            types[o[0]] = (types[o[1]][0], o[2], None)
        elif op == 43 and len(o) >= 3:
            consts[o[1]] = o[2]
        elif op == 28 and o[1] in types:
            t = types[o[1]]
            types[o[0]] = (t[0], t[1], consts.get(o[2]))
        elif op == 32:
            types[o[0]] = ('ptr', o[2], o[1])
        elif op == 59:
            var[o[1]] = (o[0], o[2])
        elif op == 71 and len(o) >= 2:
            if o[1] == 30:
                loc[o[0]] = o[2]
            elif o[1] == 32:
                index[o[0]] = o[2]
            elif o[1] in DEC_NAMES:
                decs[o[0]].add(DEC_NAMES[o[1]])
        if count == 0:
            break
        k += count
    out = {}
    for v, (ptr, sc) in var.items():
        if v not in loc or sc not in (1, 3):
            continue
        t = types.get(types.get(ptr, (None, None, None))[1])
        if t is None:
            continue
        out[('in' if sc == 1 else 'out', loc[v], index.get(v, 0))] = {
            'type': t[0], 'n': t[1], 'array': t[2], 'decorations': decs[v], 'index': index.get(v)}
    return out


def parse_pixel(words):
    """the record's 'pixel' line: key value pairs, except perspective_vgprs (center c sample s centroid t) and pos
    (the FragCoord components the program receives, possibly none: 'pos front_face 0')"""
    d, k = {}, 0
    while k < len(words):
        key = words[k]
        if key == 'perspective_vgprs':
            sub = words[k + 1:k + 7]
            for j in range(0, 6, 2):
                d['ij_' + sub[j]] = int(sub[j + 1])
            k += 7
        elif key == 'pos':
            nxt = words[k + 1] if k + 1 < len(words) else ''
            if nxt and set(nxt) <= set('xyzw'):
                d['pos'] = nxt
                k += 2
            else:
                d['pos'] = ''
                k += 1
        else:
            d[key] = words[k + 1] if k + 1 < len(words) else ''
            k += 2
    return d


def load(gres_path, spv_path=None):
    g = Interface()
    g.path = gres_path
    for line in open(gres_path):
        w = line.split()
        if not w:
            continue
        if w[0] == 'module':
            d = _kv(w[1:])
            g.stage, g.hash, g.id = d['stage'], d['hash'], d['id']
            g.user_data_base = int(d.get('user_data_base', 0))
        elif w[0] == 'output':
            d = _kv(w[2:])
            g.outputs.append((d.get('kind'), int(d.get('index', 0)), int(d.get('location', 0))))
        elif w[0] == 'input':
            d = _kv(w[2:])
            d['n'] = int(w[1])
            g.inputs.append(d)
        elif w[0] == 'fetch':
            d = _kv(w[1:])
            g.indirect_draw = d.get('indirect_draw') == '1'
            try:
                k = w.index('indirect_offsets')
                g.indirect_offsets = (int(w[k + 2]), int(w[k + 4]))
            except (ValueError, IndexError):
                pass
        elif w[0] == 'pixel':
            g.pixel = parse_pixel(w[1:])
        elif w[0] == 'target_output_mode':
            g.target_output_mode = [int(x) for x in w[1:]]
        elif w[0] == 'interpolator':
            g.interpolators.append(w[1:])
        elif w[0] == 'image':
            d = _kv(w[2:])
            d['n'] = int(w[1])
            g.images.append(d)
        elif w[0] == 'sampler':
            d = _kv(w[2:])
            d['n'] = int(w[1])
            g.samplers.append(d)
        elif w[0] == 'binding':
            g.bindings.append(_kv(w[1:]))
    g.interface = {}
    if spv_path:
        g.builtins_in, g.builtins_out = spirv_builtins(spv_path)
        g.interface = spirv_interface(spv_path)
    return g
