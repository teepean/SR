#! /bin/sh
# Build SR.exe from an SR source tree with a chosen output type, without editing SR_defs.h in that tree.
#   SR_SRC=<dir with SR sources>  (default: $HOME/sorsa/sr-mxvisor/SR, the mxvisor fork)
#   SR_OUT=OUT_X64 | OUT_LLASM     (default: OUT_X64)
#   SR_EXE=<output name>           (default: SR.exe)
# sr-x64-dos-instructions.patch (x64 backend: scasw/scasd, cmpsw/cmpsd, jecxz, more x87) is applied
# to the copy when it is not already present in the sources.
set -e
cd "`echo $0 | sed 's/\/[^\/]*$//'`"
HERE="`pwd`"
SR_SRC="${SR_SRC:-$HOME/sorsa/sr-mxvisor/SR}"
SR_OUT="${SR_OUT:-OUT_X64}"
SR_EXE="${SR_EXE:-SR.exe}"
TMP="`mktemp -d`"
cp -r "$SR_SRC"/. "$TMP/"
rm -f "$TMP"/*.o "$TMP"/SR.exe "$TMP"/.sconsign.dblite
if ! grep -q 'case UD_Iftst:' "$TMP/SR_full_x64_instr.c"; then
    (cd "$TMP" && patch -s -p2 < "$HERE/sr-x64-dos-instructions.patch")
fi
sed -i "s/^#define OUTPUT_TYPE .*/#define OUTPUT_TYPE  $SR_OUT/" "$TMP/SR_defs.h"
SCONS="${SCONS:-`command -v scons || echo $HOME/.local/share/sr-venv/bin/scons`}"
(cd "$TMP" && "$SCONS" -Q -j`nproc` >build.log 2>&1) || { tail -20 "$TMP/build.log"; exit 1; }
cp "$TMP/SR.exe" "$HERE/$SR_EXE"
rm -rf "$TMP"
echo "built $HERE/$SR_EXE ($SR_OUT from $SR_SRC)"
