#! /bin/sh
# Builds LibGodot (Godot as a shared library) for SR-AvP2's window system and renderer (host_godot.cpp), and
# godot-cpp against it.
#   build_libgodot.sh <godot checkout (4.7.2-stable)> <godot-cpp checkout (master)> [target]
# then link <godot>/bin/libgodot.linuxbsd.<target>.x86_64.so as libgodot.so next to SR-AvP2-x64 and build the
# runtime with GODOT_CPP=<godot-cpp>.
# - library_type=shared_library: LibGodot (libgodot_create_godot_instance).
# - disable_path_overrides=no: template builds ignore --path otherwise (the runtime passes its project).
# - libgodot.map: only the two libgodot_* functions are exported. Godot links libstdc++ and its third-party
#   libraries statically; exported, they interpose the host's copies or are interposed by them (FreeType,
#   zlib, ... and libstdc++'s STB_GNU_UNIQUE locale ids, which the dynamic linker unifies process-wide even
#   with RTLD_DEEPBIND - glslang crashed in istream >> double).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
GODOT="$1"; GODOT_CPP="$2"; TARGET="${3:-template_debug}"
SCONS="${SCONS:-scons}"
if [ -z "$GODOT" ] || [ -z "$GODOT_CPP" ]; then echo "usage: $0 <godot> <godot-cpp> [target]"; exit 1; fi
(cd "$GODOT" && "$SCONS" platform=linuxbsd target="$TARGET" library_type=shared_library disable_path_overrides=no \
    linkflags="-Wl,--version-script=$HERE/libgodot.map" -j"$(nproc)")
(cd "$GODOT_CPP" && "$SCONS" platform=linux target="$TARGET" api_version=4.7 -j"$(nproc)")
ls -la "$GODOT/bin/libgodot.linuxbsd.$TARGET.x86_64.so" "$GODOT_CPP/bin/"
