#! /bin/sh
#
# Recompiles 64-bit Linux executable (ELF64, not position independent, statically or dynamically linked)
# with SRW64 into new Linux executable.
#
# usage: roundtrip_elf.sh <executable> <work directory>
#
# The work directory may contain SCI files (they are used by SRW64).
# SRW64 writes linker script <name>.ld, which places the original sections (except code)
# at their original addresses and the recompiled code at a new address.
# Dynamically linked executables are linked with stub libraries created by mkstubs_elf.py
# from file <name>.imports (the real libraries are used at runtime).
#
# Result: <work directory>/<name>_srw64
#
# Environment variables:
#   SRW64 - path to SRW64 executable (default: ../SRW64.exe relative to this script)
#   LD    - linker (default: ld)
#

TESTS_DIR=$(cd "$(dirname "$0")" && pwd)
SRW64_DIR=$(dirname "$TESTS_DIR")
SRW64=${SRW64:-$SRW64_DIR/SRW64.exe}
LD=${LD:-ld}
SUPPORT_DIR=$SRW64_DIR/x64-support
REPAIR=$SRW64_DIR/../SRW64-games/template/repair_short_jumps.py

if [ $# -lt 2 ]; then
    echo "usage: $0 <executable> <work directory>"
    exit 1
fi

EXE_PATH=$1
WORK_DIR=$2
NAME=$(basename "$EXE_PATH")

mkdir -p "$WORK_DIR" || exit 1
cp "$EXE_PATH" "$WORK_DIR/$NAME" || exit 1
cd "$WORK_DIR" || exit 1

[ -f misc.inc ] || : > misc.inc
[ -f extern.inc ] || : > extern.inc

# recompile
"$SRW64" "$NAME" "$NAME.asm" > srw64.out 2> srw64.err || { echo "SRW64 failed:"; tail -5 srw64.err; exit 1; }

# assemble (short jumps which are out of range are repaired)
cp "$REPAIR" . || exit 1
PASS=0
while :; do
    nasm -f elf64 -O1 -w-number-overflow -i"$SUPPORT_DIR/" "$NAME.asm" -o "$NAME.o" 2> a.a && break
    if grep "error" a.a | grep -q -v "short jump is out of range"; then
        echo "NASM failed:"
        grep "error" a.a | head -5
        exit 1
    fi
    PASS=$((PASS + 1))
    if [ $PASS -ge 10 ]; then
        echo "NASM failed (short jumps)"
        exit 1
    fi
    python3 repair_short_jumps.py
done

# link (sections are placed by the generated linker script)
# (--eh-frame-hdr: the unwinder finds the new .eh_frame of the recompiled code using PT_GNU_EH_FRAME)
if [ -f "$NAME.imports" ]; then
    # dynamically linked executable: stub libraries with imported symbols (the real libraries are used at runtime)
    LINK_ARGS=$(python3 "$TESTS_DIR/mkstubs_elf.py" "$NAME.imports" stubs) || { echo "creating stub libraries failed"; exit 1; }
    # shellcheck disable=SC2086
    "$LD" $LINK_ARGS -E --eh-frame-hdr -T "$NAME.ld" "$NAME.o" -o "${NAME}_srw64" 2> ld.err || { cat ld.err; echo "linking failed"; exit 1; }
else
    "$LD" -static --eh-frame-hdr -T "$NAME.ld" "$NAME.o" -o "${NAME}_srw64" 2> ld.err || { cat ld.err; echo "linking failed"; exit 1; }
fi
if [ -s ld.err ]; then
    cat ld.err >&2
fi

echo "$WORK_DIR/${NAME}_srw64"
