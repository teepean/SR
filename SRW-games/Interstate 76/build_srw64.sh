#! /bin/sh
# Builds SRW with OUTPUT_TYPE OUT_X64 (needed by gen_all.sh ARCH=x64) out of tree:
# copies the SRW sources into a new directory, switches SR_defs.h to OUT_X64 and runs scons.
# Usage: build_srw64.sh <new directory>   ->   <new directory>/SRW64.exe
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRW="$HERE/../../SRW"
OUT="$1"
SCONS="${SCONS:-scons}"
if [ -z "$OUT" ] || [ -e "$OUT" ]; then echo "usage: $0 <new directory>"; exit 1; fi
mkdir -p "$OUT"
cp -r "$SRW"/*.c "$SRW"/*.cpp "$SRW"/*.h "$SRW"/SConstruct "$SRW"/udis86-1.7.2 "$OUT/"
sed -i 's/^#define OUTPUT_TYPE  OUT_X86/#define OUTPUT_TYPE  OUT_X64/' "$OUT/SR_defs.h"
grep -q '^#define OUTPUT_TYPE  OUT_X64' "$OUT/SR_defs.h"
(cd "$OUT" && "$SCONS" -Q)
mv "$OUT/SRW.exe" "$OUT/SRW64.exe"
echo "$OUT/SRW64.exe"
