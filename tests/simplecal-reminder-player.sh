#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/.local/share/simplesuite"
printf 'fake mp3\n' > "$tmp/.local/share/simplesuite/simplecal-alarm.mp3"

HOME="$tmp" "$ROOT/build/simplecal" --data-dir "$tmp/cal" >/dev/null

cat > "$tmp/cal/reminders.db" <<'REMINDERS'
EVENT_ID=due-test
DUE=2000-01-01T00:00
STATUS=pending
FIRED_AT=
FIRED_ON=

REMINDERS

cat > "$tmp/player-ok" <<'PLAYER'
#!/usr/bin/env bash
set -euo pipefail
grep '^STATUS=' "$HOME/cal/reminders.db" >> "$HOME/status-before-player"
printf '%s\n' "$1" >> "$HOME/player-path"
printf '%s\n' "$$" >> "$HOME/player-pids"
sleep 30
PLAYER
chmod +x "$tmp/player-ok"

HOME="$tmp" SIMPLECAL_ALARM_PLAYER="$tmp/player-ok" "$ROOT/build/simplecal" --check-reminders >"$tmp/check-ok.out" 2>"$tmp/check-ok.err" &
checker=$!

for _ in $(seq 1 50); do
    if grep -q '^STATUS=ringing$' "$tmp/cal/reminders.db" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

grep -q '^STATUS=ringing$' "$tmp/cal/reminders.db"

for _ in $(seq 1 50); do
    if test -s "$tmp/player-pids" && test -s "$tmp/player-path" && test -s "$tmp/status-before-player"; then
        break
    fi
    sleep 0.1
done

grep -q '^STATUS=ringing$' "$tmp/status-before-player"
grep -q "$tmp/.local/share/simplesuite/simplecal-alarm.mp3" "$tmp/player-path"

for _ in $(seq 1 50); do
    if grep -q 'started alarm player PID=' "$tmp/check-ok.err" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

grep -q 'simplecal: alarm path:' "$tmp/check-ok.err"
grep -q 'SIMPLECAL_ALARM_PLAYER=' "$tmp/check-ok.err"
grep -q 'simplecal: reminder due EVENT_ID=due-test' "$tmp/check-ok.err"
grep -q 'drift_seconds=' "$tmp/check-ok.err"
grep -q 'started alarm player PID=' "$tmp/check-ok.err"

sleep 1.5
test "$(wc -l < "$tmp/player-pids")" -eq 1

HOME="$tmp" "$ROOT/build/simplecal" --clear-reminder due-test >"$tmp/clear.out" 2>"$tmp/clear.err"

for _ in $(seq 1 50); do
    if ! kill -0 "$checker" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if kill -0 "$checker" 2>/dev/null; then
    kill "$checker" 2>/dev/null || true
    echo "checker did not exit after clear" >&2
    exit 1
fi
wait "$checker"

grep -q '^STATUS=fired$' "$tmp/cal/reminders.db"
grep -q 'simplecal: cleared 1 reminder' "$tmp/clear.out"
grep -q 'simplecal: cleared reminder EVENT_ID=due-test' "$tmp/clear.err"

cat > "$tmp/cal/reminders.db" <<'REMINDERS'
EVENT_ID=due-test
DUE=2000-01-01T00:00
STATUS=pending
FIRED_AT=
FIRED_ON=

REMINDERS

cat > "$tmp/player-fail" <<'PLAYER'
#!/usr/bin/env bash
set -euo pipefail
printf 'called\n' > "$HOME/player-fail-called"
exit 7
PLAYER
chmod +x "$tmp/player-fail"

if HOME="$tmp" SIMPLECAL_ALARM_PLAYER="$tmp/player-fail" "$ROOT/build/simplecal" --check-reminders >"$tmp/check-fail.out" 2>"$tmp/check-fail.err"; then
    echo "expected --check-reminders to fail when alarm player fails" >&2
    exit 1
fi

test -f "$tmp/player-fail-called"
grep -q '^STATUS=error$' "$tmp/cal/reminders.db"
grep -q 'exit status: 7' "$tmp/check-fail.err"
grep -q 'failure 3' "$tmp/check-fail.err"

echo "simplecal reminder player regression: ok"
