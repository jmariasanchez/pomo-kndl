#!/usr/bin/env bash
# Cross-build pomo-kndl for Kindle (armhf, glibc >= 2.19) with clang + lld against a Debian sysroot.
#   kindle/cross.sh [path-to-KOReader-libs]
# Why not zig: zig's crt and libc++ assume newer glibc and segfault at startup on the Kindle's 2.20.
# This uses the GCC 6 crt files and libstdc++ headers from Debian 9, links glibc 2.19 symbols from
# Debian 8, and resolves libstdc++/libssl/libcrypto at run time from KOReader's libs/ folder.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CACHE="${POMO_KNDL_SYSROOT:-$HOME/.cache/pomo-kndl/sysroot}"
KOLIBS="${1:-/Volumes/Kindle/koreader/libs}"
MIRROR=https://archive.debian.org/debian

# Toolchain: clang + lld (Nix by default; set CLANGXX/LLD_DIR to override).
CLANGXX="${CLANGXX:-$(nix build --no-link --print-out-paths nixpkgs#llvmPackages_21.clang-unwrapped)/bin/clang++}"
LLD_DIR="${LLD_DIR:-$(nix build --no-link --print-out-paths nixpkgs#llvmPackages_21.lld)/bin}"

STRETCH="libgtk2.0-dev libglib2.0-dev libpango1.0-dev libcairo2-dev libgdk-pixbuf2.0-dev libatk1.0-dev
libharfbuzz-dev libfreetype6-dev libpng-dev libpixman-1-dev libx11-dev x11proto-core-dev libssl1.0-dev
libgtk2.0-0 libglib2.0-0 libgdk-pixbuf2.0-0 libpcre3 libpango-1.0-0 libpangocairo-1.0-0 libpangoft2-1.0-0
libcairo2 libatk1.0-0 libfontconfig1 libx11-6 libxext6 libxrender1 libfreetype6 libpng16-16 libpixman-1-0
libxcb1 libxcb-render0 libxcb-shm0 libxau6 libxdmcp6 libexpat1 libharfbuzz0b libgraphite2-3 libthai0
libdatrie1 libxinerama1 libxi6 libxrandr2 libxcursor1 libxcomposite1 libxdamage1 libxfixes3 libffi6
libselinux1 libmount1 libblkid1 zlib1g libbsd0 libuuid1 libstdc++-6-dev libstdc++6 libgcc-6-dev libgcc1
linux-libc-dev"
# Installed last so glibc headers/libs are 2.19 (older than the Kindle's 2.20).
JESSIE="libc6 libc6-dev"

fetch() { # dist pkgs...
  local dist=$1; shift
  local idx="$CACHE/debs/Packages.$dist"
  [ -f "$idx" ] || curl -sfL "$MIRROR/dists/$dist/main/binary-armhf/Packages.gz" | gunzip > "$idx"
  for p in "$@"; do
    local u f m
    u=$(awk -v p="$p" '$1=="Package:"{x=($2==p)} x&&$1=="Filename:"{print $2; exit}' "$idx")
    [ -n "$u" ] || { echo "package $p not found in $dist"; exit 1; }
    f="$CACHE/debs/$dist-$(basename "$u")"
    [ -f "$f" ] || curl -sfL "$MIRROR/$u" -o "$f"
    m=$(ar t "$f" | grep data.tar)
    (cd "$CACHE/root" && case $m in *xz) ar p "$f" "$m" | tar xJ ;; *) ar p "$f" "$m" | tar xz ;; esac)
  done
}

mkdir -p "$CACHE/debs" "$CACHE/root" "$CACHE/kindlelibs"
if [ ! -f "$CACHE/.done-v2" ]; then
  echo "fetching armhf sysroot into $CACHE"
  fetch stretch $STRETCH
  fetch jessie $JESSIE
  touch "$CACHE/.done-v2"
fi
for l in libssl.so.60 libcrypto.so.57; do
  if [ -f "$KOLIBS/$l" ]; then cp "$KOLIBS/$l" "$CACHE/kindlelibs/"; fi
  [ -f "$CACHE/kindlelibs/$l" ] || { echo "need $l from KOReader (mount the Kindle or pass its koreader/libs path)"; exit 1; }
done
ln -sf libssl.so.60 "$CACHE/kindlelibs/libssl.so"; ln -sf libcrypto.so.57 "$CACHE/kindlelibs/libcrypto.so"

R="$CACHE/root"
INC=""
for d in usr/include/gtk-2.0 usr/lib/arm-linux-gnueabihf/gtk-2.0/include usr/include/gdk-pixbuf-2.0 \
         usr/include/pango-1.0 usr/include/cairo usr/include/atk-1.0 usr/include/glib-2.0 \
         usr/lib/arm-linux-gnueabihf/glib-2.0/include usr/include/harfbuzz usr/include/freetype2 \
         usr/include/pixman-1 usr/include/libpng16; do
  INC="$INC -isystem $R/$d"
done
FLAGS="--target=arm-linux-gnueabihf --sysroot=$R --gcc-toolchain=$R/usr -mcpu=cortex-a7 -mfpu=neon-vfpv4
 -mfloat-abi=hard -fuse-ld=lld -B$LLD_DIR --rtlib=libgcc --unwindlib=libgcc --stdlib=libstdc++ -no-pie
 -std=c++14 -O2 -s -pthread -Wl,-dynamic-linker,/lib/ld-linux-armhf.so.3 -Wl,--as-needed -Wl,--allow-shlib-undefined
 -L$CACHE/kindlelibs"
CXX() { PATH="$LLD_DIR:$PATH" "$CLANGXX" $FLAGS "$@"; }

OUT="$HERE/build-kindle"; mkdir -p "$OUT"
cd "$HERE"
CXX -o "$OUT/pomo-kndl-dev" src/dev_main.cpp src/core.cpp src/net.cpp -lssl -lcrypto -lpthread
CXX -o "$OUT/pomo-kndl-gui" src/gui_main.cpp src/core.cpp src/net.cpp $INC \
  -lgtk-x11-2.0 -lgdk-x11-2.0 -lgobject-2.0 -lglib-2.0 -lssl -lcrypto -lpthread -lpcre

rm -rf "$HERE/dist" && mkdir -p "$HERE/dist/pomo-kndl/bin"
cp "$OUT/pomo-kndl-gui" "$OUT/pomo-kndl-dev" "$HERE/dist/pomo-kndl/bin/"
cp "$HERE"/kual/* "$HERE/dist/pomo-kndl/"
chmod +x "$HERE"/dist/pomo-kndl/*.sh "$HERE"/dist/pomo-kndl/bin/*
echo "built: $HERE/dist/pomo-kndl"
