# IDAPython: dump imports with IDA's prototype -> <outdir>/imports.csv : module|name|ordinal|iat_ea|type
import os, idc, ida_nalt, ida_auto, ida_pro, ida_typeinf
ida_auto.auto_wait()
outdir = idc.ARGV[1]
rows = []
for i in range(ida_nalt.get_import_module_qty()):
    mod = ida_nalt.get_import_module_name(i)
    def cb(ea, name, ordv):
        t = idc.get_type(ea) or ""
        rows.append("%s|%s|%s|0x%x|%s" % (mod, name or "", ordv, ea, t))
        return True
    ida_nalt.enum_import_names(i, cb)
with open(os.path.join(outdir, "imports.csv"), "w") as f:
    f.write("\n".join(rows) + "\n")
ida_pro.qexit(0)
