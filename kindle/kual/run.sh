#!/bin/sh
# KUAL launcher for pomo-kndl. All state lives in this folder (state.json, pairing.json).
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/pomo-kndl.log"
export POMO_KNDL_DIR="$DIR"
# TLS comes from KOReader's bundled LibreSSL. Expose only those two libraries: putting all of
# koreader/libs on the path swaps in its libz/libpng/freetype, which breaks the firmware's GTK.
KOLIBS=/mnt/us/koreader/libs
mkdir -p "$DIR/lib"
for l in libssl.so.60 libcrypto.so.57; do
  [ -f "$KOLIBS/$l" ] && ln -sf "$KOLIBS/$l" "$DIR/lib/$l"
done
export LD_LIBRARY_PATH="$DIR/lib"
cd "$DIR" || exit 1
echo "--- $(date) run.sh $*" >>"$LOG"

if [ "$1" != diagnose ] && [ ! -f "$KOLIBS/libssl.so.60" ]; then
  echo "KOReader's libssl.so.60 not found in $KOLIBS: install or update KOReader" >>"$LOG"
  exit 1
fi

case "$1" in
  sync)
    exec "$DIR/bin/pomo-kndl-dev" "$DIR" sync >>"$LOG" 2>&1 ;;
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
    } >"$DIR/diagnose.txt" 2>&1
    exit 0 ;;
esac

export DISPLAY="${DISPLAY:-:0}"
# Detach so KUAL closing does not take the app with it.
(
  "$DIR/bin/pomo-kndl-gui" >>"$LOG" 2>&1
  echo "gui exit $?" >>"$LOG"
) </dev/null >/dev/null 2>&1 &
