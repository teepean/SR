#!/bin/sh
# builds libsrhooks.so (game-side hooks, linked into the SRW64 build) and optionally the SRW64 build itself
# usage: build.sh                      - only libsrhooks.so
#        build.sh <eboot> <work dir>   - also recompiles <eboot> with SRW64 (SCI files from sci/) into <work dir>
cd "$(dirname "$0")" || exit 1
HOOKS=$(pwd)
gcc -O2 -Wall -fPIC -shared -o libsrhooks.so srhooks.c -lpthread -ldl || exit 1
echo "libsrhooks.so built"
[ $# -lt 2 ] && exit 0
mkdir -p "$2" || exit 1
cp sci/SR.cfg sci/global_aliases.sci sci/instruction_replacements.sci sci/extern.inc libsrhooks.so "$2"/ || exit 1
WORK=$(cd "$2" && pwd)
LDFLAGS_EXTRA="-L$WORK -lsrhooks -rpath \$ORIGIN/sr --allow-shlib-undefined" \
    "$HOOKS/../../../SRW64/tests/roundtrip_elf.sh" "$1" "$WORK" || exit 1
echo "built $WORK/$(basename "$1")_srw64 (runtime: libsrhooks.so in <exe dir>/sr)"
