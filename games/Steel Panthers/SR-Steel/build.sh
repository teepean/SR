#! /bin/sh
# Build the native Linux x86-64 Steel Panthers from the recompiled STEEL.EXE and this runtime.
#   ./build.sh          incremental build of ./steelpanthers
#   ./build.sh clean
set -e
cd "`dirname "$0"`"
SRGAME="../../../SR-games/Steel Panthers/SR"
mkdir -p obj

if [ "$1" = clean ]; then rm -rf obj steelpanthers; exit 0; fi

# recompiled program (built by SR-games/Steel Panthers/SR/build-x64.sh)
if [ ! -f "$SRGAME/steel-asm.o" ]; then "$SRGAME/build-x64.sh"; fi
cp -p "$SRGAME/steel-asm.o" obj/steel-asm.o

CFLAGS="-O2 -g -Wall -Wno-unused-result -fno-pie -fno-omit-frame-pointer -fno-strict-aliasing `pkg-config --cflags sdl3`"
LIBS="`pkg-config --libs sdl3` -lm"

newer() { [ ! -f "$2" ] || [ "$1" -nt "$2" ]; }

for a in asm_int asm_inout asm_movmem asm_fs_mem sp-asm sp-functions; do
    if newer "x64/$a.asm" "obj/$a.o" || newer x64/sp-calls.inc "obj/$a.o"; then
        nasm -felf64 -Ix64/ -o "obj/$a.o" "x64/$a.asm"
    fi
done
for c in main lowmem dosfs dosint video ail; do
    if newer "$c.c" "obj/$c.o" || newer sp.h "obj/$c.o"; then
        gcc $CFLAGS -c -o "obj/$c.o" "$c.c"
    fi
done
gcc -no-pie -Wl,-Ttext-segment=0x10000000 -Wl,-z,noexecstack -o steelpanthers obj/*.o $LIBS
echo "built `pwd`/steelpanthers"
