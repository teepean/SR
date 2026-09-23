#! /bin/sh
# Regenerates the assembler versions of all recompiled Interstate '76 modules.
# Every run goes into a new directory under $WORK (default /home/teemu/sorsa/i76work/gen-NNN);
# nothing is deleted. The results are copied into games/Interstate 76/SR-I76/x86/<module>/.
#
# Needs: SRW.exe (SRW/SRW.exe of this repo), nasm, python3 and the original files in $GAME.

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
GAME="${GAME:-/home/teemu/.wine/drive_c/i76}"
WORK="${WORK:-/home/teemu/sorsa/i76work}"
DEST="$REPO/games/Interstate 76/SR-I76/x86"

n=1
while [ -e "$WORK/gen-$(printf %03d $n)" ]; do n=$((n+1)); done
RUN="$WORK/gen-$(printf %03d $n)"
mkdir -p "$RUN"
echo "output: $RUN"

gen() {
    # gen <SRW-games subdir> <original file> <module name> <extra files...>
    sub="$1"; orig="$2"; mod="$3"; shift 3
    dir="$RUN/$mod"
    mkdir "$dir"
    cp "$REPO/SRW/SRW.exe" "$GAME/$orig" "$HERE/$sub/SR.cfg" "$HERE/$sub/compact_source.py" "$dir/"
    for f in "$@"; do cp "$HERE/$sub/$f" "$dir/"; done
    for f in "$HERE/$sub/x86/"*.sci; do [ -e "$f" ] && cp "$f" "$dir/"; done
    (cd "$dir" && ./SRW.exe "$orig" "$mod.asm" > srw.out 2> srw.err; echo "$mod: SRW rc=$?"; grep -E "^Error" srw.err; python3 compact_source.py; python3 "$HERE/fix_import_names.py" "$mod.asm" seg*.inc)
    # assemble and repair out-of-range short jumps until nasm is happy
    cp "$HERE/SRW/repair_short_jumps.py" "$dir/"
    for pass in 1 2 3 4; do
        (cd "$dir" && nasm -felf32 -O1 -w+orphan-labels -w-number-overflow -i"$DEST/$mod/" -i"$DEST/" "$mod.asm" -o "$mod.o" 2> a.a)
        if ! grep -q "short jump is out of range" "$dir/a.a"; then break; fi
        (cd "$dir" && python3 repair_short_jumps.py)
    done
    grep -v "short jump is out of range" "$dir/a.a" | head -20
    cp "$dir/$mod.asm" "$dir/"seg*.inc "$DEST/$mod/"
}

gen SRW          i76.exe      i76      relocations.csv bssborder.csv
gen SRW-i76shell i76shell.dll i76shell
gen SRW-zglide   ZGLIDE.DLL   zglide
gen SRW-strlkup  STRLKUP.DLL  strlkup
