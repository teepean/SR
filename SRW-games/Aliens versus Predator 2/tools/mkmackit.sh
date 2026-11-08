#! /bin/sh
# mkmackit.sh <output directory>: the recompiled code for the macOS build (scons device=pc64-macos blob_kit=<dir>,
# or the directory mac-kit next to SConstruct): both games' images and their link tables (tools/mkblob.py), made
# from the x86-64 ELF objects of the Linux build - run `scons` in games/Aliens versus Predator 2/SR-AvP2 first.
# The images contain code generated from the game's files: keep the kit private (don't commit or publish it).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
RT="$(cd "$HERE/../../../games/Aliens versus Predator 2/SR-AvP2" && pwd)"
OUT="$1"
if [ -z "$OUT" ]; then echo "usage: $0 <output directory>"; exit 1; fi
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
cd "$RT"
COMMON="$(ls x64/*.o)"
python3 "$HERE/mkblob.py" --name SR-AvP2 --asmsyms asmsyms.h --out "$OUT" $COMMON \
    x64/lithtech/lithtech.o x64/cshell/cshell.o x64/server/server.o x64/object/object.o
if [ -e x64/ph-lithtech/lithtech.o ]; then
    python3 "$HERE/mkblob.py" --name SR-AvP2PH --asmsyms asmsyms.h --out "$OUT" $COMMON \
        x64/ph-lithtech/lithtech.o x64/ph-cshell/cshell.o x64/ph-server/server.o x64/ph-object/object.o
fi
# only what the macOS build needs (and the symbol maps, for crash addresses)
for n in SR-AvP2 SR-AvP2PH; do
    for x in .ld .thunks.asm .blob.elf; do [ -e "$OUT/$n$x" ] && rm "$OUT/$n$x"; done
done
ls -la "$OUT"
