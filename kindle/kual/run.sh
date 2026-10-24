#!/bin/sh
# KUAL launcher for pomo-kndl. All state lives in this folder (state.json, pairing.json).
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/pomo-kndl.log"
export POMO_KNDL_DIR="$DIR"
ORIG_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
# TLS comes from KOReader's bundled LibreSSL. Expose only those two libraries: putting all of
# koreader/libs on the path swaps in its libz/libpng/freetype, which breaks the firmware's GTK.
# Copy (not symlink): /mnt/us is FAT and cannot hold symlinks. Refresh if KOReader updated them.
KOLIBS=/mnt/us/koreader/libs
mkdir -p "$DIR/lib"
for l in libssl.so.60 libcrypto.so.57; do
  if [ -f "$KOLIBS/$l" ] && ! cmp -s "$KOLIBS/$l" "$DIR/lib/$l"; then
    cp "$KOLIBS/$l" "$DIR/lib/$l.tmp" && mv "$DIR/lib/$l.tmp" "$DIR/lib/$l"
  fi
done
export LD_LIBRARY_PATH="$DIR/lib${ORIG_LD_LIBRARY_PATH:+:$ORIG_LD_LIBRARY_PATH}"
cd "$DIR" || exit 1
echo "--- $(date) run.sh $*" >>"$LOG"

if [ "$1" != diagnose ] && [ ! -f "$KOLIBS/libssl.so.60" ]; then
  echo "KOReader's libssl.so.60 not found in $KOLIBS: install or update KOReader" >>"$LOG"
  exit 1
fi

case "$1" in
  sync)
    exec "$DIR/bin/pomo-kndl-dev" "$DIR" sync >>"$LOG" 2>&1 ;;
  stop)
    killall pomo-kndl-gui 2>/dev/null && echo "stopped from KUAL" >>"$LOG"
    exit 0 ;;
  probe)
    export DISPLAY="${DISPLAY:-:0}"
    "$DIR/bin/gtk-probe" >>"$LOG" 2>&1
    echo "probe exit $?" >>"$LOG"
    exit 0 ;;
  diagnose)
    {
      echo "uname: $(uname -a)"
      echo "boot_id: $(cat /proc/sys/kernel/random/boot_id 2>&1)"
      echo "uptime: $(cat /proc/uptime)"
      ls -l /lib/libc.so.6 /lib/ld-linux-armhf.so.3 2>&1
      for l in libgtk-x11-2.0.so.0 libgdk-x11-2.0.so.0 libgobject-2.0.so.0 libglib-2.0.so.0; do
        p=""; for d in /usr/lib /lib; do [ -e "$d/$l" ] && p="$d/$l" && break; done
        echo "$l: ${p:-MISSING}"
      done
      echo "ldd gui:"; ldd "$DIR/bin/pomo-kndl-gui" 2>&1 || /lib/ld-linux-armhf.so.3 --list "$DIR/bin/pomo-kndl-gui" 2>&1
      echo "koreader libssl: $(ls -l "$KOLIBS/libssl.so.60" 2>&1)"
      echo "dev state:"; "$DIR/bin/pomo-kndl-dev" "$DIR" state 2>&1
      echo "net:"; ip addr show wlan0 2>&1 | grep inet
      echo "== env from KUAL:"; echo "LD_LIBRARY_PATH(orig)=$ORIG_LD_LIBRARY_PATH"; env | grep -E "^(LD_|GTK|G_|DISPLAY|XDG)" 2>&1
      echo "== ld.so.preload:"; cat /etc/ld.so.preload 2>&1
      echo "== glib/gtk files:"; ls -la /usr/lib/libglib-2.0.so* /usr/lib/libgobject-2.0.so* /usr/lib/libgthread-2.0.so* /usr/lib/libgtk-x11-2.0.so* /usr/lib/libgdk-x11-2.0.so* /lib/libglib* 2>&1
      find / -xdev -name 'libglib-2.0.so*' 2>/dev/null
      echo "== native GTK processes (env + glib mapping):"
      for p in /proc/[0-9]*; do
        if grep -q libgtk-x11 "$p/maps" 2>/dev/null; then
          echo "-- $(cat "$p/cmdline" 2>/dev/null | tr '\0' ' ' | cut -c1-80)"
          tr '\0' '\n' <"$p/environ" 2>/dev/null | grep -E "^(LD_|GTK|G_|DISPLAY)"
          grep -E "glib|gobject|gthread|gtk-x11" "$p/maps" 2>/dev/null | awk '{print $6}' | sort -u
        fi
      done
      echo "== probe, original env:"
      ( export LD_LIBRARY_PATH="$ORIG_LD_LIBRARY_PATH" DISPLAY="${DISPLAY:-:0}"; timeout 8 "$DIR/bin/gtk-probe" 2>&1 | head -6 )
      echo "== probe LD_DEBUG=libs (which glib loads):"
      ( export DISPLAY="${DISPLAY:-:0}" LD_DEBUG=libs; timeout 8 "$DIR/bin/gtk-probe" 2>&1 | grep -E "calling init|trying file=.*(glib|gobject|gthread|gtk)" | head -20 )
      echo "== copying device GTK libs to syslibs/ for analysis"
      mkdir -p "$DIR/syslibs"
      for l in libglib-2.0.so.0 libgobject-2.0.so.0 libgthread-2.0.so.0 libgtk-x11-2.0.so.0 libgdk-x11-2.0.so.0 libgmodule-2.0.so.0 libgio-2.0.so.0; do
        f=$(readlink -f "/usr/lib/$l" 2>/dev/null); [ -f "$f" ] && cp "$f" "$DIR/syslibs/$l"
      done
      ls -la "$DIR/syslibs"
    } >"$DIR/diagnose.txt" 2>&1
    exit 0 ;;
esac

export DISPLAY="${DISPLAY:-:0}"
# One instance only: a second tap on Open would otherwise stack windows.
killall pomo-kndl-gui 2>/dev/null
# Detach so KUAL closing does not take the app with it.
(
  "$DIR/bin/pomo-kndl-gui" >>"$LOG" 2>&1
  echo "gui exit $?" >>"$LOG"
) </dev/null >/dev/null 2>&1 &
