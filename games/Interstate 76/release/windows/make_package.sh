#! /bin/sh
# Builds the Windows x64 release archive: SR-I76-windows-x64-<version>.zip (in the current directory)
# from the Windows build (games/Interstate 76/SR-I76/SR-I76.exe, built with: scons device=pc64-windows).
# SDL2DLL = path of SDL2.dll (x86_64, from the SDL2 MinGW development package).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../SR-I76"
VERSION="${VERSION:-1.0.0}"
NAME="SR-I76-windows-x64-$VERSION"
OUT="$(pwd)"
if [ -z "$SDL2DLL" ] || [ ! -f "$SDL2DLL" ]; then echo "set SDL2DLL to the path of SDL2.dll"; exit 1; fi

TMP="$(mktemp -d)"
mkdir "$TMP/$NAME"
x86_64-w64-mingw32-strip -o "$TMP/$NAME/SR-I76.exe" "$SRC/SR-I76.exe"
cp "$SDL2DLL" "$HERE/SR-I76.cfg" "$TMP/$NAME/"
sed 's/$/\r/' "$HERE/readme-Windows.txt" > "$TMP/$NAME/readme-Windows.txt"
(cd "$TMP" && zip -q -r "$OUT/$NAME.zip" "$NAME")
echo "$OUT/$NAME.zip"
echo "(temporary files left in $TMP)"
