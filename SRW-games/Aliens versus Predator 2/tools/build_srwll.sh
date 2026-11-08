#! /bin/sh
# Builds SRW with OUTPUT_TYPE OUT_LLASM (needed by gen_all_llasm.sh) out of tree: copies the SRW sources into a new
# directory, switches SR_defs.h to OUT_LLASM and runs scons.
# Usage: build_srwll.sh <new directory>   ->   <new directory>/SRWLL.exe
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRW="$HERE/../../../SRW"
OUT="$1"
SCONS="${SCONS:-scons}"
if [ -z "$OUT" ] || [ -e "$OUT" ]; then echo "usage: $0 <new directory>"; exit 1; fi
mkdir -p "$OUT"
cp -r "$SRW"/*.c "$SRW"/*.cpp "$SRW"/*.h "$SRW"/SConstruct "$SRW"/udis86-1.7.2 "$OUT/"
sed -i 's/^#define OUTPUT_TYPE  OUT_X86/#define OUTPUT_TYPE  OUT_LLASM/' "$OUT/SR_defs.h"
grep -q '^#define OUTPUT_TYPE  OUT_LLASM' "$OUT/SR_defs.h"
(cd "$OUT" && "$SCONS" -Q)
mv "$OUT/SRW.exe" "$OUT/SRWLL.exe"
echo "$OUT/SRWLL.exe"
