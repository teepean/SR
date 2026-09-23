# IDAPython: for every function, write start,name,ncallers,caller_funcs(;-sep)  -> <outdir>/func_xrefs.csv
import os, idc, idautils, ida_funcs, ida_auto, ida_pro
ida_auto.auto_wait()
outdir = idc.ARGV[1]
with open(os.path.join(outdir, "func_xrefs.csv"), "w") as f:
    for fea in idautils.Functions():
        callers = set()
        for x in idautils.XrefsTo(fea, 0):
            fn = ida_funcs.get_func(x.frm)
            callers.add("0x%x" % fn.start_ea if fn else "d0x%x" % x.frm)
        f.write("0x%x,%s,%d,%s\n" % (fea, idc.get_func_name(fea), len(callers), ";".join(sorted(callers))))
ida_pro.qexit(0)
