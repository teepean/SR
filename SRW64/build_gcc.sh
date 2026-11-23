#! /bin/sh
#
# Builds SRW64.exe with gcc/g++ (alternative to scons).
# Object files are written into the build subdirectory.
#
# Environment variables:
#   CC, CXX - compilers (default: gcc, g++)
#   CFLAGS  - additional compiler flags (for example: -g -D__DEBUG__)
#

cd "$(dirname "$0")" || exit 1

CC=${CC:-gcc}
CXX=${CXX:-g++}

mkdir -p build || exit 1

for SOURCE in *.c udis86/libudis86/*.c; do
    $CC -c -O2 -Wall -DHAVE_STRING_H=1 $CFLAGS -Iudis86 "$SOURCE" -o "build/$(basename "$SOURCE" .c).o" || exit 1
done

$CXX -c -O2 -Wall $CFLAGS -Iudis86 container_helpers.cpp -o build/container_helpers.o || exit 1

$CXX build/*.o -o SRW64.exe || exit 1

echo "SRW64.exe built"
