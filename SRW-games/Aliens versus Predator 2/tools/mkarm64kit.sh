#! /bin/sh
# mkarm64kit.sh <output directory>: the llasm version's objects for native arm64 macOS (scons device=arm64-macos
# llasm_kit=<dir>, or the directory mac-kit-arm64 next to SConstruct): SRW's llasm output of both games' modules
# (gen_all_llasm.sh, GAME=ph gen_all_llasm.sh) and the runtime's llasm files (gen_imports.py), compiled with
# llasm -ptrofs and llc for arm64-apple-macosx, so the Mac needs neither llasm nor LLVM (Xcode's clang builds the
# rest). The module objects contain code generated from the game's files: keep the kit private (don't commit or
# publish it).
HERE="$(cd "$(dirname "$0")" && pwd)"
RT="$(cd "$HERE/../../../games/Aliens versus Predator 2/SR-AvP2" && pwd)"
SUPPORT="$(cd "$HERE/../../../SR/llasm-support" && pwd)"
OUT="$1"
TRIPLE="${TRIPLE:-arm64-apple-macosx11.0.0}"
if [ -z "$OUT" ]; then echo "usage: $0 <output directory>"; exit 1; fi
mkdir -p "$OUT/llasm" || exit 1
OUT="$(cd "$OUT" && pwd)"

# build <llasm file> <object>: as llasm/SConscript does (the procedures internal: local symbols for crash reports)
build() {
    llasm -m64 -inline-idiv -inline-float -pic -ptrofs -O -I "$(dirname "$1")" -I "$RT/llasm" -I "$SUPPORT" "$1" |
        sed -e 's/^define private fastcc/define internal fastcc/' |
        llc -O=2 -filetype=obj -mtriple="$TRIPLE" --relocation-model=pic -o "$2" || echo "FAILED: $1"
}

for f in imports-asm com-asm runtime-asm; do
    build "$RT/llasm/$f.llasm" "$OUT/llasm/$f.o" &
done
for m in lithtech cshell server object; do
    build "$RT/llasm/$m/$m.llasm" "$OUT/llasm/$m.o" &
    if [ -e "$RT/llasm/ph-$m/$m.llasm" ]; then build "$RT/llasm/ph-$m/$m.llasm" "$OUT/llasm/ph-$m.o" & fi
done
wait

ok=1
for f in imports-asm com-asm runtime-asm lithtech cshell server object; do
    [ -s "$OUT/llasm/$f.o" ] || { echo "missing: llasm/$f.o"; ok=0; }
done
ls -la "$OUT/llasm"
[ $ok = 1 ]
