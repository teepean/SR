#! /bin/sh
# Regenerates the assembler versions of the recompiled Aliens versus Predator 2 modules (x86-64, SRW OUT_X64).
# Every run goes into a new directory $WORK/gen-NNN (nothing is deleted); the results are copied into
# games/Aliens versus Predator 2/SR-AvP2/x64/<module>/.
#
# SRW64=<SRW built with OUTPUT_TYPE OUT_X64> (SRW-games/Interstate 76/build_srw64.sh),
# RETAIL=<directory with lithtech.exe, server.dll, ... (CShell.DLL/Object.LTO extracted from AVP2DLL.REZ)>.
# Needs: nasm, python3.

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
I76="$REPO/SRW-games/Interstate 76"
RETAIL="${RETAIL:-/home/teemu/sorsa/avp2work/retail/avp2-eu}"
WORK="${WORK:-/home/teemu/sorsa/avp2work}"
SRWBIN="${SRW64:-/home/teemu/sorsa/avp2work/srw64-build-001/SRW64.exe}"
DEST="$REPO/games/Aliens versus Predator 2/SR-AvP2/x64"

n=1
while [ -e "$WORK/gen-$(printf %03d $n)" ]; do n=$((n+1)); done
RUN="$WORK/gen-$(printf %03d $n)"
mkdir -p "$RUN"
echo "output: $RUN"

gen() {
    # gen <SRW-games subdir> <original file> <module name>
    sub="$1"; orig="$2"; mod="$3"
    dir="$RUN/$mod"
    mkdir "$dir"
    cp "$SRWBIN" "$dir/SRW.exe"
    cp "$RETAIL/$orig" "$HERE/$sub/SR.cfg" "$I76/SRW/compact_source.py" "$I76/SRW/repair_short_jumps.py" "$dir/"
    for f in relocations.csv bssborder.csv; do [ -e "$HERE/$sub/$f" ] && cp "$HERE/$sub/$f" "$dir/"; done
    for f in "$HERE/$sub/x64/"*.sci; do [ -e "$f" ] && cp "$f" "$dir/"; done
    (cd "$dir" && ./SRW.exe "$orig" "$mod.asm" > srw.out 2> srw.err; echo "$mod: SRW rc=$?"; grep -E "^Error|disassembly error" srw.err; python3 compact_source.py > /dev/null; python3 "$I76/fix_import_names.py" "$mod.asm" seg*.inc > /dev/null)
    # assemble and repair out-of-range short jumps until nasm is happy
    for pass in 1 2 3 4 5; do
        (cd "$dir" && nasm -felf64 -O1 -w+orphan-labels -w-number-overflow -i"$DEST/$mod/" -i"$DEST/" "$mod.asm" -o "$mod.o" 2> a.a)
        if ! grep -q "short jump is out of range" "$dir/a.a"; then break; fi
        (cd "$dir" && python3 repair_short_jumps.py > /dev/null)
    done
    grep -v "short jump is out of range" "$dir/a.a" | head -20
    mkdir -p "$DEST/$mod"
    cp "$dir/$mod.asm" "$dir/"seg*.inc "$DEST/$mod/"
}

gen SRW-lithtech lithtech.exe lithtech
