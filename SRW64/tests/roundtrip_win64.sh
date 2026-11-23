#! /bin/sh
#
# Recompiles 64-bit Windows executable with SRW64 into new Windows executable (win64).
#
# usage: roundtrip_win64.sh <executable> <work directory> [console|windows]
#
# The work directory may contain SCI files (they are used by SRW64). The file
# global_aliases.sci is created by this script (entry point and TLS directory),
# additional aliases can be put into file global_aliases_extra.sci.
#
# Result: <work directory>/<name>_srw64.exe
#
# Environment variables:
#   SRW64 - path to SRW64 executable (default: ../SRW64.exe relative to this script)
#   CROSS - prefix of MinGW tools (default: x86_64-w64-mingw32-)
#

TESTS_DIR=$(cd "$(dirname "$0")" && pwd)
SRW64_DIR=$(dirname "$TESTS_DIR")
SRW64=${SRW64:-$SRW64_DIR/SRW64.exe}
CROSS=${CROSS:-x86_64-w64-mingw32-}
SUPPORT_DIR=$SRW64_DIR/x64-support
REPAIR=$SRW64_DIR/../SRW64-games/template/repair_short_jumps.py

if [ $# -lt 2 ]; then
    echo "usage: $0 <executable> <work directory> [console|windows]"
    exit 1
fi

EXE_PATH=$1
WORK_DIR=$2
SUBSYSTEM=${3:-console}
NAME=$(basename "$EXE_PATH" .exe)
NAME=$(basename "$NAME" .EXE)

mkdir -p "$WORK_DIR" || exit 1
cp "$EXE_PATH" "$WORK_DIR/$NAME.exe" || exit 1
cd "$WORK_DIR" || exit 1

[ -f misc.inc ] || : > misc.inc
[ -f extern.inc ] || : > extern.inc

# find entry point and TLS directory
: > global_aliases.sci
"$SRW64" "$NAME.exe" "$NAME.asm" > srw64.out 2> srw64.err || { echo "SRW64 failed:"; tail -5 srw64.err; exit 1; }
ENTRY=$(sed -n 's/.*Entry point: 0x\([0-9a-fA-F]*\).*/\1/p' srw64.err | tr 'a-f' 'A-F')
TLS=$(sed -n 's/.*TLS directory at 0x\([0-9a-fA-F]*\).*/\1/p' srw64.err | tr 'a-f' 'A-F')
[ -n "$ENTRY" ] && echo "loc_$ENTRY,srw_entry" >> global_aliases.sci
[ -n "$TLS" ] && echo "loc_$TLS,_tls_used" >> global_aliases.sci
[ -f global_aliases_extra.sci ] && cat global_aliases_extra.sci >> global_aliases.sci

# recompile
"$SRW64" "$NAME.exe" "$NAME.asm" > srw64.out 2> srw64.err || { echo "SRW64 failed:"; tail -5 srw64.err; exit 1; }

# assemble (short jumps which are out of range are repaired)
cp "$REPAIR" . || exit 1
PASS=0
while :; do
    nasm -f win64 -O1 -w-number-overflow -i"$SUPPORT_DIR/" "$NAME.asm" -o "$NAME.obj" 2> a.a && break
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

# import libraries and resources
LIBS=$(python3 "$TESTS_DIR/mkimplibs.py" "$NAME.asm" . "${CROSS}dlltool") || exit 1
RES=""
if [ -f "$NAME.res" ]; then
    "${CROSS}windres" -i "$NAME.res" -O coff -o "${NAME}_res.o" || exit 1
    RES="${NAME}_res.o"
fi

# link
"${CROSS}gcc" -nostartfiles -nostdlib -Wl,-e,srw_entry -Wl,--subsystem,$SUBSYSTEM "$NAME.obj" $RES -o "${NAME}_srw64.exe" $LIBS || { echo "linking failed"; exit 1; }

echo "$WORK_DIR/${NAME}_srw64.exe"
