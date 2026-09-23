# IDAPython script (IDA 9.x) run headless:
#   idat -A -c -o<db.i64> -L<log> -S"ida_export.py <outdir>" <exe>
# Exports raw material for reconstructing stripped PE relocations and for SCI files:
#   code_refs.csv  : fixup_va,value,insn_va,opnum,kind   (absolute 32-bit values inside instructions)
#   data_refs.csv  : fixup_va,value,is_off,kind,off_in_item,datatype,item_size,xrefs_to_target_from_elsewhere
#                    (every dword in data/undefined areas whose value lies inside the image, any alignment)
#   funcs.csv      : start,end,name,flags
#   heads.csv      : ea,size,kind (c=code, d=data, u=unknown) for .text only
#   segments.csv   : name,start,end
#   strings.csv    : ea,len
import os
import idc, idaapi, idautils, ida_auto, ida_bytes, ida_funcs, ida_ua, ida_segment, ida_nalt, ida_pro

ida_auto.auto_wait()

outdir = idc.ARGV[1] if len(idc.ARGV) > 1 else os.path.dirname(idc.get_idb_path())
os.makedirs(outdir, exist_ok=True)

imagebase = ida_nalt.get_imagebase()
segs = []
for s in idautils.Segments():
    seg = ida_segment.getseg(s)
    segs.append((ida_segment.get_segm_name(seg), seg.start_ea, seg.end_ea))
img_lo = min(s[1] for s in segs)
img_hi = max(s[2] for s in segs)

def in_image(v):
    return img_lo <= v < img_hi

with open(os.path.join(outdir, "segments.csv"), "w") as f:
    for n, a, b in segs:
        f.write("%s,0x%x,0x%x\n" % (n, a, b))

# functions
with open(os.path.join(outdir, "funcs.csv"), "w") as f:
    for fea in idautils.Functions():
        fn = ida_funcs.get_func(fea)
        f.write("0x%x,0x%x,%s,0x%x\n" % (fn.start_ea, fn.end_ea, idc.get_func_name(fea), fn.flags))

# code refs
insn = ida_ua.insn_t()
with open(os.path.join(outdir, "code_refs.csv"), "w") as f, open(os.path.join(outdir, "heads.csv"), "w") as fh:
    for n, a, b in segs:
        ea = a
        while ea < b and ea != idaapi.BADADDR:
            flags = ida_bytes.get_flags(ea)
            size = ida_bytes.get_item_size(ea)
            if ida_bytes.is_code(flags):
                fh.write("0x%x,%d,c\n" % (ea, size))
                if ida_ua.decode_insn(insn, ea) > 0:
                    seen = set()
                    for i in range(8):
                        op = insn.ops[i]
                        if op.type == ida_ua.o_void:
                            break
                        for offs in (op.offb, op.offo):
                            if offs <= 0 or offs in seen:
                                continue
                            if offs + 4 > insn.size:
                                continue
                            v = ida_bytes.get_dword(ea + offs)
                            # the dword must really be the operand's 32-bit field (not e.g. disp8 + imm bytes)
                            if v != (op.addr & 0xffffffff) and v != (op.value & 0xffffffff):
                                continue
                            if in_image(v) or (imagebase <= v < img_lo and op.type != ida_ua.o_imm):
                                seen.add(offs)
                                f.write("0x%x,0x%x,0x%x,%d,%d\n" % (ea + offs, v, ea, i, op.type))
            else:
                fh.write("0x%x,%d,%s\n" % (ea, size, "d" if ida_bytes.is_data(flags) else "u"))
            ea = ida_bytes.next_head(ea, b) if size else ea + 1
            if ea == idaapi.BADADDR:
                break

# data refs: scan every byte offset of non-code items
with open(os.path.join(outdir, "data_refs.csv"), "w") as f:
    for n, a, b in segs:
        ea = a
        while ea + 4 <= b:
            flags = ida_bytes.get_flags(ea)
            if ida_bytes.is_code(flags):
                ea += max(1, ida_bytes.get_item_size(ea))
                continue
            if not ida_bytes.is_loaded(ea) or not ida_bytes.is_loaded(ea + 3):
                ea += 1
                continue
            v = ida_bytes.get_dword(ea)
            if in_image(v):
                head = ida_bytes.get_item_head(ea)
                hf = ida_bytes.get_flags(head)
                is_off = 1 if (head == ea and ida_bytes.is_off0(hf)) else 0
                kind = "s" if ida_bytes.is_strlit(hf) else ("d" if ida_bytes.is_data(hf) else "u")
                dt = "-"
                if ida_bytes.is_data(hf):
                    for nm, fn in (("byte", ida_bytes.is_byte), ("word", ida_bytes.is_word), ("dword", ida_bytes.is_dword),
                                   ("qword", ida_bytes.is_qword), ("float", ida_bytes.is_float), ("double", ida_bytes.is_double),
                                   ("tbyte", ida_bytes.is_tbyte), ("struct", ida_bytes.is_struct), ("oword", ida_bytes.is_oword)):
                        if fn(hf):
                            dt = nm
                            break
                # number of references to the target from anywhere except this candidate (ordinary flow excluded)
                nx = 0
                for x in idautils.XrefsTo(v, 0):
                    if x.type == idaapi.fl_F:
                        continue
                    if head <= x.frm < head + max(1, ida_bytes.get_item_size(head)):
                        continue
                    nx += 1
                f.write("0x%x,0x%x,%d,%s,%d,%s,%d,%d\n" % (ea, v, is_off, kind, (ea - head), dt, ida_bytes.get_item_size(head), nx))
            ea += 1

with open(os.path.join(outdir, "strings.csv"), "w") as f:
    for s in idautils.Strings():
        f.write("0x%x,%d\n" % (s.ea, s.length))

ida_pro.qexit(0)
