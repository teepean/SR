# IDAPython: decompile every function into <outdir>/decompiled.c (reference only)
import os, idc, idautils, ida_auto, ida_pro, ida_hexrays, ida_funcs
ida_auto.auto_wait()
outdir = idc.ARGV[1]
ok = ida_hexrays.init_hexrays_plugin()
with open(os.path.join(outdir, "decompiled.c"), "w") as f:
    if not ok:
        f.write("// hexrays not available\n")
    else:
        for fea in idautils.Functions():
            try:
                cf = ida_hexrays.decompile(fea)
                f.write("// ---- 0x%x %s\n%s\n\n" % (fea, idc.get_func_name(fea), str(cf)))
            except Exception as e:
                f.write("// ---- 0x%x %s: decompilation failed: %s\n\n" % (fea, idc.get_func_name(fea), e))
ida_pro.qexit(0)
