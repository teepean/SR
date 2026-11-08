#! /bin/sh
# Regenerates the llasm versions of the recompiled Aliens versus Predator 2 modules (SRW OUT_LLASM: for llasm ->
# LLVM IR -> any 64-bit target, with pointer offsets for native arm64 macOS). Like gen_all.sh: every run goes into a
# new directory $WORK/llgen-NNN (nothing is deleted); the results are copied into
# games/Aliens versus Predator 2/SR-AvP2/llasm/<module>/ (GAME=ph: Primal Hunt, llasm/ph-<module>/).
#
# SRWLL=<SRW built with OUTPUT_TYPE OUT_LLASM> (tools/build_srwll.sh), RETAIL as in gen_all.sh.
# The SCI files: SRW-<module>/x64/ (the same code roots, aliases, external procedures) and SRW-<module>/llasm/
# (instruction_replacements.sci in llasm's language; instruction_flags.sci: flags across labels, made with
# tools/mkiflags.py - when SRW reports "unknown flags on start of region", run it with SRW_LIST_ERRORS=1, then
# mkiflags.py <module> srw.err SRW-<module>/llasm/instruction_flags.sci, and again until none are left).
# Then: llasm -m64 -ptrofs -inline-idiv -inline-float -pic -I SR-AvP2/llasm -I SR/llasm-support <module>.llasm,
# llc -mtriple=arm64-apple-darwin --relocation-model=pic (or x86_64-pc-linux-gnu).

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
if [ "$GAME" = ph ]; then
    RETAIL="${RETAIL:-/home/teemu/sorsa/avp2work/retail/avp2-ph-eu}"
    PFX=ph-
else
    RETAIL="${RETAIL:-/home/teemu/sorsa/avp2work/retail/avp2-eu}"
    PFX=
fi
WORK="${WORK:-/home/teemu/sorsa/avp2work}"
SRWBIN="${SRWLL:-/home/teemu/sorsa/avp2work/srwll-build/SRWLL.exe}"
DEST="$REPO/games/Aliens versus Predator 2/SR-AvP2/llasm"

# a new run directory (mkdir is atomic: parallel runs get different ones)
mkdir -p "$WORK"
n=1
while ! mkdir "$WORK/llgen-$(printf %03d $n)" 2> /dev/null; do n=$((n+1)); done
RUN="$WORK/llgen-$(printf %03d $n)"
echo "output: $RUN"

gen() {
    # gen <SRW-games subdir> <original file> <module name>
    sub="$1"; orig="$2"; mod="$3"
    dir="$RUN/$mod"
    out="$DEST/$PFX$mod"
    mkdir "$dir"
    cp "$SRWBIN" "$dir/SRW.exe"
    cp "$RETAIL/$orig" "$HERE/$sub/SR.cfg" "$dir/"
    for f in relocations.csv bssborder.csv; do [ -e "$HERE/$sub/$f" ] && cp "$HERE/$sub/$f" "$dir/"; done
    for f in external_procedures fixup_interpret_as_code global_aliases ignored_areas; do
        [ -e "$HERE/$sub/x64/$f.sci" ] && cp "$HERE/$sub/x64/$f.sci" "$dir/"
    done
    for f in "$HERE/$sub/llasm/"*.sci; do [ -e "$f" ] && cp "$f" "$dir/"; done
    (cd "$dir" && ./SRW.exe "$orig" "$mod.llasm" > srw.out 2> srw.err; echo "$mod: SRW rc=$?"; grep -E "^Error" srw.err | head -5
     # the modules export the same names (SetInstanceHandle, ...): <module>_<name>, as in gen_all.sh
     [ -e "$HERE/$sub/exports.txt" ] && python3 "$HERE/tools/prefix_exports.py" "$mod" "$HERE/$sub/exports.txt" "$mod.llasm" seg*.llinc)
    mkdir -p "$out"
    cp "$dir/$mod.llasm" "$dir/"seg*.llinc "$out/"
}

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

if [ "$GAME" = ph ]; then
    want lithtech && gen SRW-ph-lithtech lithtech.exe lithtech
    want cshell   && gen SRW-ph-cshell   CSHELL.DLL   cshell
    want server   && gen SRW-ph-server   server.dll   server
    want object   && gen SRW-ph-object   OBJECT.LTO   object
else
    want lithtech && gen SRW-lithtech lithtech.exe lithtech
    want cshell   && gen SRW-cshell   CShell.DLL   cshell
    want server   && gen SRW-server   server.dll   server
    want object   && gen SRW-object   Object.LTO   object
fi
