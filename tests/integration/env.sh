# Sourced inside dbus-run-session. Sets up an isolated shade instance.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
# Short path: the control socket must fit in sun_path (108 bytes).
W=${W:-$(mktemp -d "${TMPDIR:-/tmp}/shade-it.XXXXXX")}
mkdir -p "$W/plugins" "$W/cfg/shade" "$W/rt"
for so in "$BUILD"/plugins/*/*.so; do cp "$so" "$W/plugins/"; done
export SHADE_RUNTIME_DIR=$W/rt SHADE_PLUGIN_PATH=$W/plugins SHADE_DEBUG=1 QT_QPA_PLATFORM=offscreen
export XDG_CONFIG_HOME=$W/cfg SHADE_STATS_BUS=session FAKE_PACTL_STATE=$W/pactl
export SHADE_SYSFS_NET=$W/sysnet; mkdir -p "$W/sysnet"
cat > "$W/cfg/shade/config.toml" <<TOML
[shade]
peek_timeout_ms = 60000

["plugin:stats"]
pactl = "$ROOT/tests/integration/fake-pactl"
network_backend = "iwd"
network_command = "sh -c 'echo net-tool; exec cat'"
bluetooth_command = "sh -c 'echo bye; sleep 0.3'"
volume_command = "/nonexistent/pulsemixer"
terminal_width = 480
terminal_height = 200
TOML
C="$BUILD/shadectl/shadectl"
mock() { gdbus call --session -d org.shade.Mocks -o /test -m "org.shade.Test.$1" "${@:2}" >/dev/null; }
