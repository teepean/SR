#
# IDAPython script for headless IDA Pro (9.x): exports information for comparison with SRW64
# (compare_ida.py) or decompiles all functions (Hex-Rays).
#
# usage (see compare_ida.sh):
#   TVHEADLESS=1 SRW64_IDA_OUT=<output directory> idat -A -L<log> -S<path>/ida_export.py -o<database>.i64 <executable>
#
# Environment variables:
#   SRW64_IDA_OUT     - output directory (default: directory of the database)
#   SRW64_IDA_MODE    - "analyze" (default): waits for the auto-analysis, writes the files below and saves the database
#                       "decompile": decompiles functions into decompiled.c (or decompiled_NN.c)
#   SRW64_IDA_WORKERS, SRW64_IDA_WORKER - decompile mode: number of parallel IDA processes (each with its own
#                       copy of the database) and index of this process; every process decompiles a contiguous
#                       part of the sorted list of functions (concatenated files are sorted by address)
#
# Files written in mode "analyze" (addresses are IDA addresses, hexadecimal):
#   ida_info.txt      - image base, input file
#   segments.txt      - <start> <end> <permissions> <name>
#   functions.txt     - <start> <name> <thunk|-> <chunk start>-<chunk end> ...      (end exclusive)
#   code_ranges.txt   - <start> <end>     instructions inside functions (end exclusive)
#   switches.txt      - <jump> <table> <base of elements> <element size> <number of cases> <number of targets> <targets>
#
# ELF files without section headers for .eh_frame (e.g. converted executables): IDA finds the FDEs only in segments
# named .eh_frame_hdr and .eh_frame, so the segments are created from the program header PT_GNU_EH_FRAME
# and functions are created at the starts of all FDEs before the auto-analysis.
#

import os
import struct
import time
import traceback

import ida_allins
import ida_auto
import ida_bytes
import ida_funcs
import ida_ida
import ida_loader
import ida_nalt
import ida_pro
import ida_segment
import ida_ua
import ida_xref
import idautils
import idc

MODE = os.environ.get("SRW64_IDA_MODE", "analyze")
WORKERS = int(os.environ.get("SRW64_IDA_WORKERS", "1"))
WORKER = int(os.environ.get("SRW64_IDA_WORKER", "0"))
OUT = os.environ.get("SRW64_IDA_OUT") or os.path.dirname(os.path.abspath(ida_loader.get_path(ida_loader.PATH_TYPE_IDB)))

log_file = open(os.path.join(OUT, "ida_export_%s_%d.log" % (MODE, WORKER)), "a")


def log(message):
    log_file.write(time.strftime("%H:%M:%S ") + message + "\n")
    log_file.flush()


def s32(value):
    return value - (1 << 32) if value & 0x80000000 else value


def make_segment(start, end, name):
    old = ida_segment.getseg(start)
    old_start, old_end, perm = old.start_ea, old.end_ea, old.perm
    old_name, old_class = ida_segment.get_segm_name(old), ida_segment.get_segm_class(old)
    if not ida_segment.add_segm(0, start, end, name, "CONST", ida_segment.ADDSEG_NOSREG | ida_segment.ADDSEG_QUIET):
        log("unable to create segment %s" % name)
        return
    # parts of the original segment before and after the new segment
    for seg_start, seg_end, seg_name, seg_class in ((start, end, name, "CONST"), (old_start, start, old_name, old_class), (end, old_end, old_name, old_class)):
        if seg_start >= seg_end:
            continue
        if ida_segment.getseg(seg_start) is None:
            ida_segment.add_segm(0, seg_start, seg_end, seg_name, seg_class, ida_segment.ADDSEG_NOSREG | ida_segment.ADDSEG_QUIET)
        seg = ida_segment.getseg(seg_start)
        if seg is not None:
            ida_segment.set_segm_addressing(seg, 2)
            seg.perm = perm
            seg.update()
    log("segment %s: %x - %x" % (name, start, end))


def prepare_eh_frame():
    path = ida_nalt.get_input_file_path()
    if not path or not os.path.isfile(path):
        return
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return
    if ida_segment.get_segm_by_name(".eh_frame") is not None or ida_segment.get_segm_by_name(".eh_frame_hdr") is not None:
        return

    e_type, = struct.unpack_from("<H", data, 0x10)
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    hdr = hdr_size = None
    min_vaddr = None
    for index in range(phnum):
        p_type, = struct.unpack_from("<I", data, phoff + index * phentsize)
        p_vaddr, = struct.unpack_from("<Q", data, phoff + index * phentsize + 16)
        if p_type == 1 and (min_vaddr is None or p_vaddr < min_vaddr):
            min_vaddr = p_vaddr
        if p_type == 0x6474e550:
            hdr = p_vaddr
            hdr_size, = struct.unpack_from("<Q", data, phoff + index * phentsize + 40)
    if hdr is None:
        return

    # position independent executable: the address in IDA = virtual address + image base
    if e_type == 3:
        hdr += ida_nalt.get_imagebase() - ((min_vaddr or 0) & ~0xfff)
    if ida_segment.getseg(hdr) is None:
        log("eh_frame_hdr not in segment: %x" % hdr)
        return
    if tuple(ida_bytes.get_byte(hdr + index) for index in range(4)) != (1, 0x1b, 3, 0x3b):
        log("unsupported encoding of eh_frame_hdr")
        return

    eh_frame = hdr + 4 + s32(ida_bytes.get_dword(hdr + 4))
    count = ida_bytes.get_dword(hdr + 8)
    starts = [hdr + s32(ida_bytes.get_dword(hdr + 12 + 8 * index)) for index in range(count)]

    # end of .eh_frame: terminator (length 0)
    address = eh_frame
    seg_end = ida_segment.getseg(eh_frame).end_ea
    while address < seg_end:
        length = ida_bytes.get_dword(address)
        if length == 0:
            address += 4
            break
        address += (12 + ida_bytes.get_qword(address + 4)) if length == 0xffffffff else (4 + length)

    make_segment(hdr, hdr + hdr_size, ".eh_frame_hdr")
    make_segment(eh_frame, address, ".eh_frame")
    log("eh_parse: %s" % ida_loader.load_and_run_plugin("eh_parse", 0))
    for start in starts:
        ida_auto.auto_make_proc(start)
    log("functions from FDEs: %d" % count)


def write_analysis():
    with open(os.path.join(OUT, "ida_info.txt"), "w") as w:
        w.write("imagebase 0x%x\n" % ida_nalt.get_imagebase())
        w.write("input %s\n" % ida_nalt.get_input_file_path())

    with open(os.path.join(OUT, "segments.txt"), "w") as w:
        for index in range(ida_segment.get_segm_qty()):
            seg = ida_segment.getnseg(index)
            perm = "".join(c if seg.perm & bit else "-" for c, bit in (("r", ida_segment.SEGPERM_READ), ("w", ida_segment.SEGPERM_WRITE), ("x", ida_segment.SEGPERM_EXEC)))
            w.write("0x%x 0x%x %s %s\n" % (seg.start_ea, seg.end_ea, perm, ida_segment.get_segm_name(seg)))

    num_functions = num_instructions = num_switches = 0
    with open(os.path.join(OUT, "functions.txt"), "w") as wf, \
         open(os.path.join(OUT, "code_ranges.txt"), "w") as wc, \
         open(os.path.join(OUT, "switches.txt"), "w") as ws:
        for function in idautils.Functions():
            # imported functions (segment extern)
            seg = ida_segment.getseg(function)
            if seg is None or seg.type == ida_segment.SEG_XTRN:
                continue
            chunks = list(idautils.Chunks(function))
            thunk = (ida_funcs.get_func(function).flags & ida_funcs.FUNC_THUNK) != 0
            wf.write("0x%x %s %s %s\n" % (function, ida_funcs.get_func_name(function), "thunk" if thunk else "-", " ".join("0x%x-0x%x" % chunk for chunk in chunks)))
            num_functions += 1

            for chunk_start, chunk_end in chunks:
                run_start = run_end = None
                for address in idautils.Heads(chunk_start, chunk_end):
                    if not ida_bytes.is_code(ida_bytes.get_flags(address)):
                        continue
                    num_instructions += 1
                    size = ida_bytes.get_item_size(address)
                    if run_end == address:
                        run_end = address + size
                    else:
                        if run_start is not None:
                            wc.write("0x%x 0x%x\n" % (run_start, run_end))
                        run_start, run_end = address, address + size

                    insn = ida_ua.insn_t()
                    if ida_ua.decode_insn(insn, address) and insn.itype == ida_allins.NN_jmpni:
                        switch = ida_nalt.get_switch_info(address)
                        if switch is None:
                            continue
                        targets = sorted(set(x.to for x in idautils.XrefsFrom(address, ida_xref.XREF_FAR) if x.iscode and x.type == ida_xref.fl_JN))
                        ws.write("0x%x 0x%x 0x%x %d %d %d %s\n" % (address, switch.jumps, switch.elbase, switch.get_jtable_element_size(),
                                                                  switch.get_jtable_size(), len(targets), " ".join("0x%x" % t for t in targets)))
                        num_switches += 1
                if run_start is not None:
                    wc.write("0x%x 0x%x\n" % (run_start, run_end))

    log("functions: %d, instructions: %d, switches: %d" % (num_functions, num_instructions, num_switches))


def decompile():
    import ida_hexrays

    if not ida_hexrays.init_hexrays_plugin():
        log("decompiler is not available")
        return 1

    functions = sorted(idautils.Functions())
    first = len(functions) * WORKER // WORKERS
    last = len(functions) * (WORKER + 1) // WORKERS
    name = "decompiled.c" if WORKERS == 1 else "decompiled_%02d.c" % WORKER
    done = failed = 0
    with open(os.path.join(OUT, name), "w") as w:
        for function in functions[first:last]:
            try:
                cfunc = ida_hexrays.decompile(function, None, ida_hexrays.DECOMP_NO_CACHE)
                text = str(cfunc) if cfunc is not None else "/* decompilation failed */"
            except Exception as e:
                text = "/* decompilation failed: %s */" % str(e).replace("*/", "* /")
                failed += 1
            w.write("/* ==== function 0x%x %s ==== */\n%s\n\n" % (function, ida_funcs.get_func_name(function), text))
            done += 1
            if done % 1000 == 0:
                log("decompiled: %d" % done)
                w.flush()
    log("decompiled: %d (failed: %d)" % (done, failed))
    return 0


def main():
    log("start, mode %s" % MODE)
    if MODE == "analyze":
        prepare_eh_frame()
    ida_auto.auto_wait()
    log("auto-analysis finished")

    if MODE == "analyze":
        write_analysis()
        ida_loader.save_database(None, 0)
        return 0
    if MODE == "decompile":
        return decompile()
    log("unknown mode: %s" % MODE)
    return 1


try:
    code = main()
except Exception:
    log(traceback.format_exc())
    code = 1
log("done (%d)" % code)
ida_pro.qexit(code)
