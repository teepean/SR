#!/usr/bin/env python3
# nocontract.py <in.spv> <out.spv> - decorates every float add, subtract, multiply, divide and negate of a SPIR-V
# module NoContraction: GCN/RDNA round v_mul_f32 and v_add_f32 separately (only v_fma / v_fmac are fused), while
# GLSL lets the driver fuse a multiply and an add into an FMA. The decorations go at the end of the annotation
# section (before the first type declaration).
import struct
import sys

OP_DECORATE = 71
DECORATION_NO_CONTRACTION = 42
FLOAT_OPS = {
    127: 'OpFNegate', 129: 'OpFAdd', 131: 'OpFSub', 133: 'OpFMul', 136: 'OpFDiv', 141: 'OpFRem', 142: 'OpVectorTimesScalar',
    143: 'OpMatrixTimesScalar', 145: 'OpMatrixTimesVector', 146: 'OpMatrixTimesMatrix', 148: 'OpDot', 140: 'OpFMod',
}
# first type-declaration opcodes (OpTypeVoid .. OpTypeForwardPointer, OpTypeCooperativeMatrix* are later)
TYPE_OPS = set(range(19, 40)) | {322, 327, 4456, 5341}
ANNOTATION_OPS = {71, 72, 73, 74, 75, 332, 5632, 5633}   # OpDecorate, OpMemberDecorate, groups, DecorateId, ...


def main():
    data = open(sys.argv[1], 'rb').read()
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    header, body = words[:5], words[5:]
    insts = []
    i = 0
    while i < len(body):
        count = body[i] >> 16
        insts.append(body[i:i + count])
        i += count
    decorated = set()
    for inst in insts:
        if (inst[0] & 0xffff) == OP_DECORATE and len(inst) >= 3 and inst[2] == DECORATION_NO_CONTRACTION:
            decorated.add(inst[1])
    targets = []
    for inst in insts:
        op = inst[0] & 0xffff
        if op in FLOAT_OPS and len(inst) >= 3 and inst[2] not in decorated:
            targets.append(inst[2])       # result id (word 2: result type, result id)
    # insert after the last annotation instruction (or before the first type)
    at = None
    for k, inst in enumerate(insts):
        op = inst[0] & 0xffff
        if op in ANNOTATION_OPS:
            at = k + 1
        elif op in TYPE_OPS and at is None:
            at = k
            break
        elif op in TYPE_OPS:
            break
    new = [[(3 << 16) | OP_DECORATE, rid, DECORATION_NO_CONTRACTION] for rid in targets]
    insts[at:at] = new
    out = header + [w for inst in insts for w in inst]
    with open(sys.argv[2], 'wb') as f:
        f.write(struct.pack('<%dI' % len(out), *out))
    print('%s: NoContraction on %d float operations' % (sys.argv[2], len(targets)))


if __name__ == '__main__':
    main()
