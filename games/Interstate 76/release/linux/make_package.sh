#! /bin/sh
# Builds the Linux x64 release archive: SR-I76-linux-x64-<version>.tar.gz (in the current directory)
# from the x64 build (games/Interstate 76/SR-I76/SR-I76-x64, built with: scons device=pc64-linux).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../SR-I76"
VERSION="${VERSION:-1.0.0}"
NAME="SR-I76-linux-x64-$VERSION"
OUT="$(pwd)"

TMP="$(mktemp -d)"
mkdir "$TMP/$NAME"
strip -o "$TMP/$NAME/SR-I76" "$SRC/SR-I76-x64"
cp "$HERE/I76.sh" "$HERE/SR-I76.cfg" "$HERE/x64/readme-Linux.txt" "$TMP/$NAME/"
chmod +x "$TMP/$NAME/SR-I76" "$TMP/$NAME/I76.sh"
tar -C "$TMP" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "$OUT/$NAME.tar.gz"
echo "(temporary files left in $TMP)"
