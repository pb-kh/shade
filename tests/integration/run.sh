#!/usr/bin/env bash
# Headless end-to-end test. Run inside a private bus:
#   dbus-run-session -- bash tests/integration/run.sh
# Mock UPower/iwd/BlueZ/MPRIS services, a fake pactl and notify-send drive
# a real shade (offscreen platform); shadectl observes and injects input.
set -u
source "$(dirname "$0")/env.sh"
FAILS=0
pass() { echo "ok   - $*"; }
fail() { echo "FAIL - $*"; FAILS=$((FAILS + 1)); }
# expect <description> <timeout s> <command...>: retries until it succeeds.
expect() {
  local what=$1 t=$2; shift 2
  local end=$(( $(date +%s%N) + t * 1000000000 ))
  while :; do
    if eval "$@" >/dev/null 2>&1; then pass "$what"; return 0; fi
    [ "$(date +%s%N)" -gt "$end" ] && { fail "$what"; echo "     state: $($C state 2>&1 | tr '\n' ' ')"; return 1; }
    sleep 0.1
  done
}
state() { $C state --json; }
has_peek() { state | python3 -c "import json,sys; d=json.load(sys.stdin); sys.exit(0 if any(p['plugin']=='$1' and p['id']=='$2' for w in d['windows'] for p in w['peeks']) else 1)"; }
no_peeks() { state | python3 -c "import json,sys; d=json.load(sys.stdin); sys.exit(0 if all(not w['peeks'] for w in d['windows']) else 1)"; }
win_is() { state | python3 -c "import json,sys; w=json.load(sys.stdin)['windows'][0]; sys.exit(0 if w['state']=='$1' and ('${2:-}'=='' or w['selected']=='${2:-}') else 1)"; }
loaded() { state | python3 -c "import json,sys; sys.exit(0 if sorted(p['name'] for p in json.load(sys.stdin)['plugins'])==['media','notifications','stats'] else 1)"; }
MAIN=$BASHPID
cleanup() { [ "$BASHPID" = "$MAIN" ] || return; $C stop >/dev/null 2>&1; [ -n "${MOCKS:-}" ] && kill "$MOCKS" 2>/dev/null; [ -n "${KEEP:-}" ] && echo "artifacts in $W" || rm -rf "$W"; }
trap cleanup EXIT

python3 - "$W/art.png" <<'PY'
import sys, zlib, struct
w = h = 64
rows = b''.join(b'\0' + b''.join(bytes((x * 4, 255 - y * 4, 128)) for x in range(w)) for y in range(h))
c = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + c(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + c(b'IDAT', zlib.compress(rows)) + c(b'IEND', b''))
PY
python3 "$ROOT/tests/integration/mocks.py" "file://$W/art.png" > "$W/mocks.log" 2>&1 &
MOCKS=$!
expect "mock services up" 5 "gdbus introspect --session -d org.shade.Mocks -o /test"

# --- process control -------------------------------------------------------
expect "shadectl start" 10 "$C start"
expect "status reports running" 2 "$C status | grep -q running"
expect "second start reports the running instance" 2 "$C start | grep -q 'already running'"
expect "ping" 2 "$C ping"
expect "all three plugins loaded" 5 loaded
expect "panes in file-name order" 5 "state | python3 -c \"import json,sys; sys.exit(0 if [p['plugin'] for p in json.load(sys.stdin)['windows'][0]['panes']]==['stats','media','notifications'] else 1)\""
sleep 2.2 # past the media plugin's startup quiet period
expect "no peeks for the initial state" 1 no_peeks

# --- stats peeks -----------------------------------------------------------
mock Battery 81.0 1
expect "battery charging peeks" 3 "has_peek stats battery"
mock Wifi false
expect "wifi disconnect peeks" 3 "has_peek stats network"
mock BtConnect true
expect "bluetooth connect peeks" 3 "has_peek stats bluetooth"
echo "65 no" > "$FAKE_PACTL_STATE"
expect "external volume change peeks" 3 "has_peek stats volume"
$C dismiss stats
expect "dismiss <plugin> clears its peeks" 2 no_peeks
mock BtPower false
expect "bluetooth power-off peeks" 3 "has_peek stats bluetooth"
$C dismiss

# --- stats widget ------------------------------------------------------------
$C focus stats
expect "focus stats shows the live widget" 3 "win_is widgets stats"
expect "render stats" 3 "$C render $W/stats.png && test -s $W/stats.png"
# Paging: a wheel's positive dx advances, a touchpad swipe the other way.
$C wheel 120 0; $C wheel 0 0 --end
expect "wheel advances stats -> media" 3 "win_is widgets media"
$C wheel 120 0 --touchpad; $C wheel 0 0 --end
expect "touchpad +dx goes back media -> stats (reversed)" 3 "win_is widgets stats"
$C collapse
expect "collapse returns to the bar" 3 "win_is bar"

# --- embedded terminals -------------------------------------------------------
term() { state | python3 -c "import json,sys; t=[t for w in json.load(sys.stdin)['windows'] for t in w['terminals'] if t['owner']=='$1']; print(json.dumps(t[0]) if t else '')"; }
term_field() { term "$1" | python3 -c "import json,sys; d=sys.stdin.read().strip(); print(json.loads(d)['$2'] if d else '')"; }
no_terminals() { state | python3 -c "import json,sys; sys.exit(0 if all(not w['terminals'] for w in json.load(sys.stdin)['windows']) else 1)"; }
$C signal stats launch '{"what":"network"}'
expect "stats launch opens an embedded terminal (and expands onto stats)" 3 "[ \"\$(term_field stats state)\" = presented ] && win_is widgets stats"
expect "the tool's output shows in the terminal" 3 "term_field stats text | grep -q net-tool"
expect "terminal motion settles on its final size" 3 "term stats | python3 -c \"import json,sys; t=json.load(sys.stdin); sys.exit(0 if abs(t['rect']['w']-480)<1 and abs(t['rect']['h']-200)<1 else 1)\""
$C type 'hello shade\n' --owner stats
expect "typing reaches the process (cat echoes)" 3 "[ \"\$(term_field stats text | grep -c 'hello shade')\" = 2 ]"
$C render "$W/terminal.png"
PID1=$(term_field stats pid)
$C signal stats launch '{"what":"network"}'
expect "a second request replaces the terminal (one per plugin)" 3 "[ -n \"\$(term_field stats pid)\" ] && [ \"\$(term_field stats pid)\" != $PID1 ] && [ \$(state | grep -c '\"owner\": \"stats\"') = 1 ]"
expect "the replaced process is gone" 3 "! kill -0 $PID1"
PID2=$(term_field stats pid)
$C wheel 120 0; $C wheel 0 0 --end
expect "paging away (deactivation) retracts and removes the terminal" 3 no_terminals
expect "its process is terminated" 3 "! kill -0 $PID2"
$C focus stats
$C signal stats launch '{"what":"network"}'
expect "relaunch" 3 "[ \"\$(term_field stats state)\" = presented ]"
PID3=$(term_field stats pid)
$C collapse
expect "collapsing retracts the terminal" 3 no_terminals
expect "and terminates it" 3 "! kill -0 $PID3"
$C signal stats launch '{"what":"bluetooth"}'
expect "a command that exits..." 3 "term_field stats text | grep -q bye"
expect "...retracts its terminal" 4 no_terminals
$C signal stats launch '{"what":"volume"}'
sleep 0.5
expect "a command that cannot start leaves no terminal" 1 no_terminals
expect "spawn failure is logged" 2 "grep -q 'terminal for stats failed' $SHADE_RUNTIME_DIR/shade.log"
$C signal shade terminal_request '{"command":"cat","width":10,"height":200}'
sleep 0.3
expect "an unusable size is rejected" 1 no_terminals
$C signal shade terminal_request '{"command":"cat","width":300,"height":120}'
expect "shadectl can host a terminal too" 3 "[ \"\$(term_field shadectl state)\" = presented ]"
$C signal shade terminal_close
expect "terminal_close retracts it" 3 no_terminals
$C collapse

# --- media ---------------------------------------------------------------------
mock Track "'Second Song'" "'Other Artist'" "'file://$W/art.png'" "''"
expect "track change peeks" 3 "has_peek media track"
$C render "$W/media-peek.png"
# Hovering a media peek expands it into the media widget.
$C pointer 320 25
expect "hover expands the media peek into its widget" 3 "win_is widgets media"
sleep 0.8
$C click 354 85; sleep 0.2; $C click 392 85; sleep 0.2
expect "play/pause and next reach the player" 3 "gdbus call --session -d org.shade.Mocks -o /test -m org.shade.Test.Calls | grep -q \"'PlayPause', 'Next'\""
$C signal media control '{"action":"shuffle"}'
expect "bus signal toggles shuffle" 3 "gdbus call --session -d org.shade.Mocks -o /test -m org.shade.Test.Calls | grep -q 'Set Shuffle 1'"
$C leave
expect "leaving morphs back into the still-queued peek" 3 "win_is peek && has_peek media track"
$C wheel 0 60; $C wheel 0 0 --end
expect "scroll-up retires the media peek" 3 "win_is bar"

# --- notifications -------------------------------------------------------------
dbus-monitor --session "type='signal',interface='org.freedesktop.Notifications'" > "$W/signals.log" 2>&1 &
MON=$!
sleep 0.3
notify-send -a Mail "Hello" "first"
expect "a notification peeks (expand on top)" 3 "has_peek notifications n-1"
$C pointer 320 25; sleep 0.6
expect "hovering a notification peek does not expand" 1 "win_is peek"
$C pointer 320 0
expect "pointer at the top expands the shade" 3 "win_is widgets"
$C collapse; $C leave
gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications -m org.freedesktop.Notifications.Notify chat 0 "" Ping "with action" '["default","Open"]' '{}' 0 >/dev/null
expect "a second notification peeks" 3 "has_peek notifications n-2"
$C wheel 0 60; $C wheel 0 0 --end
expect "scroll-up retires the presented peek" 3 "! has_peek notifications n-2"
$C focus notifications
sleep 1.2
$C click 320 30 # top card = "Ping": invokes the default action and dismisses
expect "click invokes ActionInvoked(default)" 3 "grep -A2 'member=ActionInvoked' $W/signals.log | grep -q '\"default\"'"
expect "click closes with reason 2" 3 "grep -A2 'member=NotificationClosed' $W/signals.log | grep -q 'uint32 2'"
gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications -m org.freedesktop.Notifications.CloseNotification 1 >/dev/null
expect "CloseNotification closes with reason 3" 3 "grep -A2 'member=NotificationClosed' $W/signals.log | grep -B1 'uint32 3' | grep -q 'uint32 1'"
expect "GetServerInformation" 2 "gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications -m org.freedesktop.Notifications.GetServerInformation | grep -q \"'1.2'\""
expect "second daemon cannot take the name" 2 "python3 -c \"import dbus,sys; sys.exit(0 if dbus.SessionBus().request_name('org.freedesktop.Notifications', dbus.bus.NAME_FLAG_DO_NOT_QUEUE)==3 else 1)\""
$C signal notifications dnd '{"on":true}'
notify-send "held"
sleep 0.5
expect "do-not-disturb holds peeks" 1 "! state | grep -q 'n-3'"
$C collapse
kill $MON 2>/dev/null

# --- teardown --------------------------------------------------------------------
expect "stop" 6 "$C stop"
expect "runtime files removed" 2 "! test -e $SHADE_RUNTIME_DIR/shade.pid && ! test -e $SHADE_RUNTIME_DIR/shade.sock"
if grep -E "\[(crit|fatal)\]" "$SHADE_RUNTIME_DIR/shade.log"; then fail "no critical log lines"; else pass "no critical log lines"; fi

echo "$FAILS failure(s)"
exit $(( FAILS > 0 ))
