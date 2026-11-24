#!/bin/sh
# builds libysr.so (LD_PRELOAD interposer for the game's sceAgc* imports)
cd "$(dirname "$0")" || exit 1
python3 gen_tramp.py agc_imports.txt || exit 1
gcc -O2 -Wall -fPIC -mgeneral-regs-only -c ysr_hook.c -o ysr_hook.o || exit 1
gcc -O2 -Wall -fPIC -c ysr.c -o ysr.o || exit 1
gcc -fPIC -c ysr_tramp.S -o ysr_tramp.o || exit 1
gcc -shared -o libysr.so ysr_tramp.o ysr_hook.o ysr.o -ldl -lpthread || exit 1
echo "libysr.so built"
