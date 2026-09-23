#! /bin/sh
# Generates the x86 assembler version of i76.exe (Interstate '76, GOG version).
# Put SRW.exe and i76.exe into this directory and run this script.
cd "`echo $0 | sed 's/\/[^\/]*$//'`"
cp x86/*.sci ./
./SRW.exe i76.exe i76.asm >a.a 2>b.a
./compact_source.py
nasm -felf32 -O1 -w+orphan-labels -w-number-overflow -ix86/ i76.asm 2>a.a
./repair_short_jumps.py
