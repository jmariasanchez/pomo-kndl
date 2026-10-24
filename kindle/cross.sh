#!/usr/bin/env bash
# Cross-build pomo-kndl for Kindle (armhf, firmware 5.16.3+) with zig. No Docker needed.
#   kindle/cross.sh [path-to-KOReader-libs]
# Headers come from Debian 9 armhf packages (glibc 2.24, GTK 2.24). Binaries target glibc 2.17
# and link GTK 2 from the Kindle firmware and LibreSSL from KOReader's libs/ at run time.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CACHE="${POMO_KNDL_SYSROOT:-$HOME/.cache/pomo-kndl/sysroot}"
KOLIBS="${1:-/Volumes/Kindle/koreader/libs}"
MIRROR=https://archive.debian.org/debian
TARGET=arm-linux-gnueabihf.2.17
PKGS="libgtk2.0-dev libglib2.0-dev libpango1.0-dev libcairo2-dev libgdk-pixbuf2.0-dev libatk1.0-dev
libharfbuzz-dev libfreetype6-dev libpng-dev libpixman-1-dev libx11-dev x11proto-core-dev libssl1.0-dev
libgtk2.0-0 libglib2.0-0 libgdk-pixbuf2.0-0 libpcre3 libpcre3-dev libpango-1.0-0 libpangocairo-1.0-0
libpangoft2-1.0-0 libcairo2 libatk1.0-0 libfontconfig1 libfontconfig1-dev libx11-6 libxext6 libxrender1
libfreetype6 libpng16-16 libpixman-1-0 libxcb1 libxcb-render0 libxcb-shm0 libxau6 libxdmcp6 libexpat1
libharfbuzz0b libgraphite2-3 libthai0 libdatrie1 libxinerama1 libxi6 libxrandr2 libxcursor1 libxcomposite1
libxdamage1 libxfixes3 libffi6 libselinux1 libmount1 libblkid1 zlib1g libbsd0 libuuid1"

command -v zig >/dev/null || { echo "zig is required (brew install zig)"; exit 1; }
mkdir -p "$CACHE/debs" "$CACHE/root" "$CACHE/kindlelibs"
if [ ! -f "$CACHE/.done" ]; then
  echo "fetching armhf headers into $CACHE"
  curl -sfL "$MIRROR/dists/stretch/main/binary-armhf/Packages.gz" | gunzip > "$CACHE/debs/Packages"
  for p in $PKGS; do
    u=$(awk -v p="$p" '$1=="Package:"{x=($2==p)} x&&$1=="Filename:"{print $2; exit}' "$CACHE/debs/Packages")
    [ -n "$u" ] || { echo "package $p not found"; exit 1; }
    f="$CACHE/debs/$(basename "$u")"
    [ -f "$f" ] || curl -sfL "$MIRROR/$u" -o "$f"
    (cd "$CACHE/root" && { ar p "$f" data.tar.xz 2>/dev/null | tar xJ 2>/dev/null || ar p "$f" data.tar.gz | tar xz; })
  done
  touch "$CACHE/.done"
fi
for l in libssl.so.60 libcrypto.so.57; do
  if [ -f "$KOLIBS/$l" ]; then cp "$KOLIBS/$l" "$CACHE/kindlelibs/"; fi
  [ -f "$CACHE/kindlelibs/$l" ] || { echo "need $l from KOReader (mount the Kindle or pass its koreader/libs path)"; exit 1; }
done
ln -sf libssl.so.60 "$CACHE/kindlelibs/libssl.so"; ln -sf libcrypto.so.57 "$CACHE/kindlelibs/libcrypto.so"

R="$CACHE/root"; L="$R/usr/lib/arm-linux-gnueabihf"
INC=""
for d in usr/include/gtk-2.0 usr/lib/arm-linux-gnueabihf/gtk-2.0/include usr/include/gdk-pixbuf-2.0 \
         usr/include/pango-1.0 usr/include/cairo usr/include/atk-1.0 usr/include/glib-2.0 \
         usr/lib/arm-linux-gnueabihf/glib-2.0/include usr/include/harfbuzz usr/include/freetype2 \
         usr/include/pixman-1 usr/include/libpng16 usr/include usr/include/arm-linux-gnueabihf; do
  INC="$INC -I$R/$d"
done
CXX="zig c++ -target $TARGET -mcpu=cortex_a7 -std=c++17 -O2 -s"
LIBS="-L$CACHE/kindlelibs -L$L -L$R/lib/arm-linux-gnueabihf -Wl,--as-needed -Wl,--allow-shlib-undefined"
OUT="$HERE/build-kindle"; mkdir -p "$OUT"
cd "$HERE"
$CXX -o "$OUT/pomo-kndl-dev" src/dev_main.cpp src/core.cpp src/net.cpp $INC $LIBS -lssl -lcrypto
$CXX -o "$OUT/pomo-kndl-gui" src/gui_main.cpp src/core.cpp src/net.cpp $INC $LIBS \
  -lgtk-x11-2.0 -lgdk-x11-2.0 -lgobject-2.0 -lglib-2.0 -lpcre -lssl -lcrypto

rm -rf "$HERE/dist" && mkdir -p "$HERE/dist/pomo-kndl/bin"
cp "$OUT/pomo-kndl-gui" "$OUT/pomo-kndl-dev" "$HERE/dist/pomo-kndl/bin/"
cp "$HERE"/kual/* "$HERE/dist/pomo-kndl/"
chmod +x "$HERE"/dist/pomo-kndl/*.sh "$HERE"/dist/pomo-kndl/bin/*
echo "built: $HERE/dist/pomo-kndl"
file "$OUT"/pomo-kndl-* 2>/dev/null || true
