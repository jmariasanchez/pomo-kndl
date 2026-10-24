#!/bin/sh
# KUAL launcher for pomo-kndl. All state lives in this folder (state.json, pairing.json).
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/pomo-kndl.log"
export POMO_KNDL_DIR="$DIR"
# TLS comes from KOReader's bundled LibreSSL; GTK 2 comes from the firmware.
KOLIBS=/mnt/us/koreader/libs
mkdir -p "$DIR/lib"
for l in libssl.so.60 libcrypto.so.57; do
  [ -e "$DIR/lib/$l" ] || cp "$KOLIBS/$l" "$DIR/lib/$l" 2>>"$LOG"
done
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
cd "$DIR" || exit 1
echo "--- $(date) run.sh $*" >>"$LOG"

case "$1" in
  sync)
    exec "$DIR/bin/pomo-kndl-dev" "$DIR" sync >>"$LOG" 2>&1 ;;
  diagnose)
    {
      echo "uname: $(uname -a)"
      echo "boot_id: $(cat /proc/sys/kernel/random/boot_id 2>&1)"
      echo "uptime: $(cat /proc/uptime)"
      ls -l /lib/libc.so.6 /lib/ld-linux-armhf.so.3 2>&1
      for l in libgtk-x11-2.0.so.0 libgdk-x11-2.0.so.0 libgobject-2.0.so.0 libglib-2.0.so.0; do
        p=$(ls /usr/lib/$l /lib/$l 2>/dev/null | head -1); echo "$l: ${p:-MISSING}"
      done
      echo "dev state:"; "$DIR/bin/pomo-kndl-dev" "$DIR" state 2>&1
      echo "net:"; ip addr show wlan0 2>&1 | grep inet
    } >"$DIR/diagnose.txt" 2>&1
    exit 0 ;;
esac

# Prevent the screensaver from suspending mid-session is a device policy we do not change.
"$DIR/bin/pomo-kndl-gui" >>"$LOG" 2>&1
echo "gui exit $?" >>"$LOG"
