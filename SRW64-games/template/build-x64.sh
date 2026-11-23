#! /bin/sh
# Generates assembler version of the original executable and assembles it.
# usage: build-x64.sh [elf64|win64]
# Put SRW64.exe and the original executable into this directory and set NAME.
cd "`echo $0 | sed 's/\/[^\/]*$//'`"
NAME=game
# original executable (Windows: $NAME.exe, Linux: $NAME)
INPUT=$NAME.exe
FORMAT=${1:-elf64}
cp x64/*.sci ./ 2>/dev/null
./SRW64.exe $INPUT $NAME.asm >a.a 2>b.a
rm -f ./*.sci
./compact_source.py
cp x64/misc.inc x64/extern.inc ./
for PASS in 1 2 3 4 5 6 7 8 9 10; do
    nasm -f$FORMAT -O1 -w+orphan-labels -w-number-overflow -i../../SRW64/x64-support/ $NAME.asm -o $NAME.$FORMAT.o 2>a.a && break
    ./repair_short_jumps.py
done
cat a.a
