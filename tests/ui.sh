#!/usr/bin/env bash
# Real GTK widgets and Rust TLS server, with isolated state. Requires GTK 2 and jq.
# Pass an output directory to regenerate screenshots, e.g. bash tests/ui.sh docs/screens.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH=/usr/bin:$PATH
W="$(mktemp -d "${JCODE_SCRATCH_DIR:-${TMPDIR:-/tmp}}/pomo-ui.XXXXXX")"
OUT="${1:-$W/screens}"
mkdir -p "$OUT" "$W/empty" "$W/invalid" "$W/demo" "$W/pages" "$W/sync" "$W/max-title"
OUT="$(cd "$OUT" && pwd)"
SERVER=""
trap 'if [ -n "$SERVER" ]; then kill "$SERVER" 2>/dev/null || true; wait "$SERVER" 2>/dev/null || true; fi' EXIT
make -s -C "$ROOT/kindle" gui CXX="${CXX:-/usr/bin/c++}" >"$W/build.log" 2>&1 || { cat "$W/build.log"; exit 1; }
GUI="$ROOT/kindle/build/pomo-kndl-gui"
run() {
  POMO_KNDL_DIR="$W/$1" POMO_KNDL_GEOMETRY="${GEOMETRY:-618x824}" POMO_KNDL_SCRIPT="$2" "$GUI"
}

run empty "expect:No tasks yet;fit;shot:$OUT/empty.png;tap:Sync with computer;expect:Not paired;tap:Sync status;expect:Computer sync;fit;shot:$OUT/unpaired.png;tap:Sync now;expect:Not paired;tap:Back;expect:Your tasks"
echo 'ok - first-run guidance and sync remain available'

printf '{invalid' > "$W/invalid/pairing.json"
run invalid "tap:Sync with computer;expect:Check pairing;tap:Sync status;expect:Computer sync;fit;shot:$OUT/pairing-error.png;tap:Sync now;expect:Check pairing;tap:Sync now;expect:Check pairing;tap:Back;fit"
echo 'ok - malformed pairing is actionable and does not latch syncing'

jq -n '{proto:1,host:"127.0.0.1",port:1,server_sha256:"00",device_id:"preview",token:"preview"}' > "$W/demo/pairing.json"
jq -n '{last_sync:"synced",plan_version:1,tasks:[
  {id:"outline",title:"Write project outline",position:1},
  {id:"notes",title:"Review chapter 3 notes",position:2,done_locally:true},
  {id:"reply",title:"Reply to Ana about the launch plan",position:3},
  {id:"sync",title:"Refactor sync engine",position:4}
]}' > "$W/demo/state.json"
run demo "expect:Your tasks;expect:Last sync OK;fit;shot:$OUT/list.png;tap:Write project outline;expect:Focusing;fit;shot:$OUT/focus.png;key:Pause;expect:Paused;fit;shot:$OUT/paused.png;tap:Sync status;expect:Computer sync;key:Back;expect:Paused;key:Resume;expect:Focusing;tap:End session;expect:Session ended early;fit;shot:$OUT/end.png;tap:Take a break;expect:Take a break;fit;shot:$OUT/break.png;tap:End break;expect:Your tasks;tap:Review chapter 3 notes;expect:Your tasks"
jq -e '.session.phase == "idle" and ([.log[].kind] | index("reopen") != null)' "$W/demo/state.json" > /dev/null
echo 'ok - focus, pause, resume, end, break, reopen and connection navigation'

cp "$W/demo/pairing.json" "$W/pages/pairing.json"
LONG='Review the complete implementation plan and outstanding questions before preparing the next project milestone for discussion'
jq -n --arg title "$LONG" '{last_sync:"ok",tasks:([{id:"long",title:$title,review:true}] + [range(2;12) | {id:(tostring),title:("Task " + tostring)}])}' > "$W/pages/state.json"
run pages "expect:Review;expect:1 / 3;fit;shot:$OUT/pages.png;tap:Next;expect:2 / 3;fit;tap:Next;expect:3 / 3;absent:Next;tap:Previous;expect:2 / 3;tap:Task 8;expect:Focusing;tap:End session;tap:Complete task;expect:Your tasks;tap:Previous;expect:1 / 3;tap:$LONG;expect:Focusing;fit;shot:$OUT/long-title.png;tap:End session;fit;tap:Keep open;expect:Your tasks"
jq -e '[.log[] | select(.kind == "done" and .task_id == "8")] | length == 1' "$W/pages/state.json" > /dev/null
echo 'ok - all carried tasks reachable, long titles and review badges fit'

cp "$W/demo/pairing.json" "$W/max-title/pairing.json"
MAX_TITLE="$(jq -nr '"W" * 200')"
jq -n --arg title "$MAX_TITLE" '{tasks:[{id:"max",title:$title,review:true}]}' > "$W/max-title/state.json"
run max-title "fit;tap:$MAX_TITLE;expect:Focusing;fit;tap:End session;expect:Session ended early;fit;tap:Keep open;expect:Your tasks"
echo 'ok - maximum-length unbroken title leaves every session action visible'

# Exercise an actual GUI landing without reading or changing the real user's pairing/state.
cargo build -q --manifest-path "$ROOT/cli/Cargo.toml"
CLI="$ROOT/cli/target/debug/pomo-kndl"
PORT=$((20000 + RANDOM % 20000))
"$CLI" --home "$W/home" add 'GUI landing task' > /dev/null
"$CLI" --home "$W/home" pair --host 127.0.0.1 --port "$PORT" --out "$W/sync/pairing.json" > /dev/null
"$CLI" --home "$W/home" serve --bind "127.0.0.1:$PORT" >"$W/server.log" 2>&1 &
SERVER=$!
for _ in {1..50}; do
  if rg -q 'listening on' "$W/server.log"; then break; fi
  sleep 0.1
done
rg -q 'listening on' "$W/server.log"
run sync "tap:Sync with computer;wait-sync;expect:Last sync OK;expect:GUI landing task;tap:GUI landing task;tap:End session;tap:Complete task;expect:2 to sync;tap:Sync status;expect:2 saved changes waiting to sync;tap:Sync now;wait-sync;expect:Last sync OK;expect:0 saved changes waiting to sync;fit;shot:$OUT/connection.png;tap:Back;expect:No tasks yet"
"$CLI" --home "$W/home" tasks | rg '\[x\].*GUI landing task' > /dev/null
"$CLI" --home "$W/home" history | rg interrupted > /dev/null
"$CLI" --home "$W/home" add 'Added after last sync' > /dev/null
POMO_KNDL_AUTO_SYNC=1 run sync "wait-sync;expect:Last sync OK;expect:Added after last sync;fit"
echo 'ok - normal startup fetches computer changes with an empty local log'
kill "$SERVER"; wait "$SERVER" 2>/dev/null || true; SERVER=""
run sync "tap:Sync with computer;wait-sync;expect:Sync failed;tap:Sync status;expect:Computer sync;fit;shot:$OUT/connection-error.png;tap:Sync now;wait-sync;expect:Sync failed;tap:Back;expect:Added after last sync"
echo 'ok - real pinned TLS landing, durable completion, failed connection and retry'

# Script assertions must fail rather than silently generating misleading screenshots.
if run empty 'tap:Does not exist' >"$W/missing.log" 2>&1; then
  echo 'FAIL: missing target was accepted'; exit 1
fi
if run empty 'expect:Does not exist' >"$W/assertion.log" 2>&1; then
  echo 'FAIL: missing text was accepted'; exit 1
fi
if command -v magick > /dev/null; then
  magick "$OUT/list.png" "$OUT/focus.png" "$OUT/end.png" "$OUT/break.png" -resize 400x533 -bordercolor '#bbbbbb' -border 1 +append -depth 8 "$OUT/overview.png"
fi
printf 'ui: all checks passed; screenshots: %s; fixtures/logs: %s\n' "$OUT" "$W"
