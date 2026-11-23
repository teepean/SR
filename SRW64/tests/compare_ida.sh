#! /bin/sh
#
# Compares jump tables and decoded code of SRW64 with the analysis of IDA Pro (headless).
#
# usage: compare_ida.sh <executable> <work directory>
#
# The executable is copied into the work directory, SRW64 is run there (output <name>.asm and the lists
# srw64_jump_tables.txt, srw64_code_ranges.txt; SR.cfg and SCI files in the work directory are used),
# IDA analyzes the executable (database and exported files in <work directory>/ida - the analysis is
# reused when ida/switches.txt exists) and compare_ida.py prints the differences.
#
# Environment variables:
#   SRW64 - path to SRW64 executable (default: ../SRW64.exe relative to this script)
#   IDAT  - path to IDA's text mode executable idat (default: idat from PATH)
#
# Exit code: 0 - no differences, 1 - differences (check them, IDA is not always right), 2 - error
#

TESTS_DIR=$(cd "$(dirname "$0")" && pwd)
SRW64=${SRW64:-$(dirname "$TESTS_DIR")/SRW64.exe}
IDAT=${IDAT:-idat}

if [ $# -lt 2 ]; then
    echo "usage: compare_ida.sh <executable> <work directory>"
    exit 2
fi

EXE=$1
NAME=$(basename "$EXE")
mkdir -p "$2/ida" || exit 2
WORK=$(cd "$2" && pwd)
EXE_DIR=$(cd "$(dirname "$EXE")" && pwd)

if [ "$EXE_DIR" != "$WORK" ]; then
    cp "$EXE" "$WORK/$NAME" || exit 2
fi
cd "$WORK" || exit 2

echo "SRW64: $NAME"
if ! "$SRW64" --list_jump_tables=srw64_jump_tables.txt --list_code_ranges=srw64_code_ranges.txt "$NAME" "${NAME%.*}.asm" > srw64.out 2> srw64.err; then
    echo "SRW64 failed, see $WORK/srw64.err"
    exit 2
fi

if [ ! -f ida/switches.txt ]; then
    echo "IDA: analysis of $NAME"
    TVHEADLESS=1 SRW64_IDA_OUT="$WORK/ida" SRW64_IDA_MODE=analyze "$IDAT" -A -L"$WORK/ida/ida.log" -S"$TESTS_DIR/ida_export.py" -o"$WORK/ida/$NAME.i64" "$WORK/$NAME" > ida/idat.out 2>&1
    if [ ! -f ida/switches.txt ]; then
        echo "IDA analysis failed, see $WORK/ida/ida.log and $WORK/ida/ida_export_analyze_0.log"
        exit 2
    fi
fi

python3 "$TESTS_DIR/compare_ida.py" "$NAME" srw64_jump_tables.txt srw64_code_ranges.txt ida
