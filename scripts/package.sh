#!/bin/sh
# Package release archives into dist/.
#
#   scripts/package.sh VERSION [BINDIR...]
#
# Two archives per OS, from whichever binaries exist in the BINDIRs (default:
# build/ and gui/target/release/):
#   memslammer-VERSION-OS-x64       CLI + lite GUI, kept as small as possible
#   memslammer-gui-VERSION-OS-x64   the full GPU-rendered app
# Windows gets .zip, Linux .tar.xz. Plus SHA256SUMS.txt.
set -eu
v=$1; shift
root=$(cd "$(dirname "$0")/.." && pwd)
[ $# -gt 0 ] || set -- "$root/build" "$root/gui/target/release"
dirs="$*"
out=$root/dist
rm -rf "$out"
mkdir -p "$out"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

find_bin() {
    for d in $dirs; do [ -f "$d/$1" ] && { echo "$d/$1"; return 0; }; done
    return 1
}

# pack NAME OS EXT README BINARY...
pack() {
    name=$1-$v-$2-x64; os=$2; ext=$3; readme=$4; shift 4
    dir=$stage/$name
    mkdir -p "$dir"
    for b in "$@"; do
        f=$(find_bin "$b$ext") || { echo "skip $name: no $b$ext"; rm -rf "$dir"; return 0; }
        cp "$f" "$dir/$b$ext"
    done
    cp "$root/LICENSE" "$dir/LICENSE.txt"
    sed "s/@VERSION@/$v/" "$root/scripts/$readme" > "$dir/README.txt"
    if [ "$readme" = README-gui.txt ]; then
        if [ -n "${THIRD_PARTY:-}" ] && [ -f "$THIRD_PARTY/$os.txt" ]; then
            cp "$THIRD_PARTY/$os.txt" "$dir/THIRD-PARTY-LICENSES.txt"
        else
            echo "warning: no third-party license file for $os (set THIRD_PARTY)" >&2
        fi
    fi
    if [ "$ext" = .exe ]; then
        (cd "$stage" && zip -qr9X "$out/$name.zip" "$name")
    else
        chmod 755 "$dir"/memslammer*
        tar -C "$stage" --owner=0 --group=0 -cJf "$out/$name.tar.xz" "$name"
    fi
}

pack memslammer     windows .exe README-minimal.txt memslammer memslammer-lite
pack memslammer-gui windows .exe README-gui.txt     memslammer-gui
pack memslammer     linux   ""   README-minimal.txt memslammer memslammer-lite
pack memslammer-gui linux   ""   README-gui.txt     memslammer-gui
(cd "$out" && sha256sum -- * > SHA256SUMS.txt)
ls -l "$out"
