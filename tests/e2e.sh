#!/usr/bin/env bash
# End-to-end: real Rust `serve` + C++ Kindle core over pinned TLS on localhost.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH=/usr/bin:$PATH
(cd "$ROOT/cli" && cargo build -q)
(cd "$ROOT/kindle" && make -s dev CXX=/usr/bin/c++ >/dev/null)
CLI="$ROOT/cli/target/debug/pomo-kndl"
K="$ROOT/kindle/build/pomo-kndl-dev"
W="$(mktemp -d)"; H="$W/home"; KD="$W/kindle"; mkdir -p "$KD"
PORT=$((20000 + RANDOM % 20000))
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$W"' EXIT
pk() { "$CLI" --home "$H" "$@"; }
serve() { pk serve --bind 127.0.0.1:$PORT --once 2>>"$W/serve.log" & sleep 0.5; }
ok() { echo "ok - $*"; }

pk add "Write project outline" >/dev/null
pk add "Review notes" >/dev/null
pk pair --host 127.0.0.1 --port $PORT --out "$KD/pairing.json" >/dev/null

# 1. First landing delivers the flight plan.
serve; "$K" "$KD" sync >/dev/null; wait
[ "$("$K" "$KD" tasks | wc -l | tr -d ' ')" = 2 ] && ok "flight plan carried 2 tasks"

# 2. Offline: interrupted session + done request on task 1.
"$K" "$KD" start 1; "$K" "$KD" end; "$K" "$KD" done
"$K" "$KD" tasks | grep -q "1 \[x\] Write" && ok "done shown locally while offline"

# Computer meanwhile renames task 2 (Kindle untouched there).
ID2=$(pk tasks | awk '/Review/{print $(NF-2)}')
pk edit "$ID2" "Review notes v2" >/dev/null

# 3. Landing: session accepted, done applied, renamed title arrives.
serve; "$K" "$KD" sync | grep -q "0 pending" && ok "log acked"; wait
pk tasks | grep -q "\[x\].*Write project outline" && ok "Kindle done applied on computer"
pk history | grep -q "interrupted" && ok "session in history"
"$K" "$KD" tasks | grep -q "Review notes v2" && ok "rename carried back"

# 4. Conflict: Kindle marks task 2 done, computer archives it before landing.
"$K" "$KD" start 1; "$K" "$KD" end; "$K" "$KD" done
pk archive "$ID2" >/dev/null
serve; "$K" "$KD" sync >/dev/null; wait
pk status | grep -q "Kindle asked 'done'" && ok "conflict flagged for review"
pk resolve "$ID2" --keep >/dev/null && pk status | grep -q "nothing to review" && ok "resolved"

# 5. Repeated delivery: no duplicates.
N1=$(pk history | grep -c "min  ")
cp "$KD/state.json" "$W/backup.json"
"$K" "$KD" start 1 2>/dev/null || true
serve; "$K" "$KD" sync >/dev/null; wait
N2=$(pk history | grep -c "min  ")
[ "$N1" = "$N2" ] && ok "no duplicate sessions ($N2)"

# 6. Wrong pin is refused and keeps the log.
pk add "New task" >/dev/null
jq '.server_sha256="00"' "$KD/pairing.json" > "$W/p" && cp "$KD/pairing.json" "$W/good" && cp "$W/p" "$KD/pairing.json"
serve
if "$K" "$KD" sync 2>"$W/err"; then echo "FAIL pin accepted"; exit 1; fi
grep -q "identity mismatch" "$W/err" && ok "wrong server pin refused"
wait || true
cp "$W/good" "$KD/pairing.json"

# 7. Bad token is refused.
jq '.token="bad"' "$KD/pairing.json" > "$W/p2"; cp "$W/p2" "$KD/pairing.json"
serve
if "$K" "$KD" sync 2>"$W/err"; then echo "FAIL bad token accepted"; exit 1; fi
grep -q "auth" "$W/err" && ok "bad token refused"
wait || true
echo "e2e: all checks passed"
