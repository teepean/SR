#! /bin/sh
# Recompile STEEL.EXE (Steel Panthers v1.2, md5 702ee2143a12c8b344a95d3d3a596ca4) to x64 assembler.
# v1.1/ keeps the v1.1 executable and its SCI files (tools/port_sci.py translated them).
cd "`echo $0 | sed 's/\/[^\/]*$//'`"
[ -x SR.exe ] || ./build-sr-x64.sh || exit 1
cp x64/*.sci ./
./SR.exe STEEL.EXE steel-asm.asm >a.a 2>b.a
RES=$?
rm -f *.sci
if [ $RES -ne 0 ]; then tail -5 b.a; exit $RES; fi
./compact_source.py
# assemble; each pass widens short jumps that went out of range, the last pass must be clean
for pass in 1 2 3; do
    nasm -felf64 -O1 -w+orphan-labels -w-number-overflow -ix64/ steel-asm.asm 2>a.a && break
    ./repair_short_jumps.py
done
if grep -q error a.a; then grep error a.a | head; exit 1; fi
echo "steel-asm.o: `nm -u steel-asm.o | wc -l` undefined runtime symbols"
