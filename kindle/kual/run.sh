#!/bin/sh
# KUAL launcher for pomo-kndl. All state lives in this folder (state.json, pairing.json).
DIR="$(cd "$(dirname "$0")" && pwd)"
export POMO_KNDL_DIR="$DIR"
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
cd "$DIR" || exit 1
if [ "$1" = "sync" ]; then
  exec "$DIR/bin/pomo-kndl-dev" "$DIR" sync >>"$DIR/pomo-kndl.log" 2>&1
fi
# Keep the screen from sleeping only while the app is in front is device policy; we do not change it.
"$DIR/bin/pomo-kndl-gui" >>"$DIR/pomo-kndl.log" 2>&1
