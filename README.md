# shade

A small Wayland top-of-screen shade built with Qt 6 Widgets (layer-shell via
LayerShellQt). At rest it is a hairline bar at the top center of every
monitor. Hovering the edge springs it open into a rounded pill that holds one
plugin pane at a time. You scroll sideways between panes, and plugins can
surface short **peeks** while the shade is collapsed.

- **Plugins** are Qt plugins loaded at startup that talk to the shade and to
  each other over an asynchronous **signal bus**.
- **Peeks** are compact presentations of a plugin's own widget, not separate
  widgets. Depending on its `expand` policy, a peek morphs into the full
  widget on hover (media), or only when the pointer is pushed to the screen
  top (notifications).
- **Animation**: everything is a damped spring, so every animation can be
  interrupted and retargeted. Paging moves one pane per gesture, with a
  rubber band that lets the next-but-one pane peek in only a little, and the
  pill's size bounces with the strip.
- **Icons** are SVGs that morph geometrically: contours are matched by id and
  resampled, e.g. play ↔ pause, shuffle off ↔ on, Wi-Fi bars, battery levels.

## Bundled plugins

| Plugin | Pane | Peeks |
| --- | --- | --- |
| `stats` | clock, network, Bluetooth, battery, volume. Hover reveals details (SSID, devices); scrolling over the volume item changes the volume; clicking opens `iwctl` / `bluetuith` / `pulsemixer` in an embedded terminal below the shade; middle click mutes | battery charging / discharging / full / low; network connected / disconnected; Bluetooth on / off and device connect / disconnect (with device battery); volume changes made by other apps |
| `media` | MPRIS player: album art (file, http(s), data URIs, YouTube thumbnail fallback) over a blurred backdrop; morphing play/pause, shuffle and loop; seek bar | the new title on track change; hover morphs it into the widget, and the thumbnail travels to its place |
| `notifications` | freedesktop notification daemon: cards newest first (five, then "+x more." and "clear all"), critical styling, ages, staggered dismissal wave, do-not-disturb | every notification (priority follows urgency). Hovering does nothing; pushing to the top expands the shade |

Network backends: **iwd** (station state, SSID, signal strength), or
NetworkManager, with wired links detected from sysfs. `auto` prefers iwd.
Bluetooth talks to BlueZ through its ObjectManager, so adapters and devices
that appear later, power toggles and per-device `Battery1` levels are all
tracked. Battery comes from UPower's DisplayDevice. Volume uses `pactl`
(PulseAudio or PipeWire).

## Embedded terminals

Plugins can ask the shade for an interactive terminal running a command
(`terminal_request`). The stats plugin uses this for `iwctl`, `bluetuith`
and `pulsemixer`. The terminal sits 10 px below the pill in the shade's own
surface, with rounded corners and 5 px padding. It grows out of the pill with
the expand bounce, follows the pill as it pages or resizes, and retracts back
into it when the shade collapses, when you page away from the owning plugin,
when the plugin closes it, or when the command exits. The process (and its
process group) is terminated whenever the terminal goes away.

- The emulator is libvterm (xterm-256color, truecolor, alternate screen,
  mouse reporting, bracketed paste, reflow). Colours come from the theme.
- While a terminal is shown, the shade takes the keyboard
  (`terminal_keyboard = "exclusive"`, the default; `"on_demand"` waits
  for a click). Ctrl+Shift+V / Shift+Insert paste; Shift+PageUp/Down and the
  wheel scroll back.
- The pointer moving from the pill into the terminal never counts as
  leaving, so the shade stays open while you use it.
- Without libvterm on the system, Meson downloads and builds the 0.3.3
  release (`-Dterminal=disabled` builds without terminals; stats then falls
  back to an external terminal emulator).

## Build

Requirements: Qt 6 (Widgets, DBus, Network), Meson, and optionally
LayerShellQt (without it you get a plain window, which is useful for
development). toml++ and libvterm come from wraps when the system has none.

```sh
meson setup build
meson compile -C build
meson install -C build        # shade, shadectl, SDK, plugins, example config
```

Options: `-Dlayer_shell=enabled|disabled|auto`, `-Dterminal=enabled|disabled|auto`, `-Dtests=true|false`.

## Running and `shadectl`

```sh
shadectl start                 # background daemon (log: $XDG_RUNTIME_DIR/shade/shade.log)
shadectl status | stop | restart
shadectl state [--json]        # windows, panes, queued peeks
shadectl expand | collapse [--window <screen>]
shadectl focus <plugin>        # expand and show that plugin's pane
shadectl dismiss [<plugin> [<peek id>]]
shadectl signal <dest> <name> ['<json>']   # put a signal on the bus
shadectl plugins list | install <name|file.so> [--as base] | remove <name>
```

Examples of plugin bus APIs:

```sh
shadectl signal media control '{"action":"play-pause"}'   # next, previous, shuffle, loop
shadectl signal notifications dnd '{"on":true}'
shadectl signal notifications clear
shadectl signal stats test_peek '{"kind":"battery"}'
shadectl signal stats launch '{"what":"network"}'          # bluetooth, volume
```

Plugins load from `$SHADE_PLUGIN_PATH` if it is set, otherwise from the user
directory (`$XDG_STATE_HOME/shade/plugins`, where `shadectl plugins install`
puts them) and then the system directory (`<libdir>/shade/plugins`). A user
plugin overrides a system plugin of the same name, and panes are ordered by
file name (hence `10-stats.so`, `20-media.so`, …).

Debug commands (`debug_commands = true` or `SHADE_DEBUG=1`) inject input and
render frames without touching your session: `shadectl pointer <x> <y>`,
`click`, `leave`, `wheel <dx> <dy> [--touchpad] [--end]`, `type <text>`
(into a terminal), `render <file.png>`.

Paging direction: a mouse wheel's (or tilt wheel's) positive horizontal
delta advances; touchpad swipes are reversed (`touchpad_scroll_direction`).

## Configuration

`$XDG_CONFIG_HOME/shade/config.toml`. All keys are optional; see
[`data/config.example.toml`](data/config.example.toml), which lists every
key with its default. The sections are `[shade]` (geometry, paging, springs,
timeouts, `disabled_plugins`, `terminal`), `[theme]` (palette overrides) and
one `["plugin:<name>"]` section per plugin, handed to that plugin as a
`QVariantMap`:

```toml
[shade]
corner_radius = 20
expand_damping = 0.55          # bouncier

["plugin:stats"]
network_backend = "iwd"
show = ["time", "network", "battery"]

["plugin:stats".peeks]
volume = false
```

Bad values are logged with the file path and replaced by their defaults.

## Writing a plugin

Link against `shade-sdk` (`pkg-config shade-sdk`; plugins go into
`$(pkg-config --variable=plugindir shade-sdk)`), implement `shade::Plugin`,
and ship a `meta.json` with `name`, `author`, `description`, `version` and
`api` (`"1.0"`: the major version must match, the minor must be ≤ the
host's).

```cpp
class Hello : public QObject, public shade::Plugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ShadePlugin_iid FILE "meta.json")
    Q_INTERFACES(shade::Plugin)
public:
    bool load(shade::Host *host) override {
        host->bus()->handle("widget_request", [host](const shade::Signal &s) {
            auto *pane = new HelloPane;               // a shade::PaneWidget
            host->bus()->send("shade", "widget",
                {{"widget", QVariant::fromValue<QObject *>(pane)}}, s.window);
        });
        host->bus()->send("shade", "peek",
            shade::peekPayload("hi", {{"text", "hello"}}, shade::PeekPriority::Medium, "hover"));
        return true;
    }
    void unload() override {}
};
```

The bus protocol:

| Direction | Signal | Payload |
| --- | --- | --- |
| shade → plugin | `widget_request` | per window (`signal.window` = screen name) |
| | `activate` / `deactivate` | the plugin's pane became visible / hidden (start and stop polling here) |
| | `peek_dismissed` | `{id, reason}` (`timeout`, `user`, `plugin`, …) |
| plugin → shade | `widget` | `{widget}`: a `shade::PaneWidget`, one per window |
| | `peek` | `peekPayload(id, data, priority, expand, timeoutMs)`; re-publishing an id updates it in place; `timeoutMs` −1 means the default, 0 means never |
| | `dismiss_peek` | `{id}` (empty id dismisses all of this plugin's peeks) |
| | `activate_widget` | expand the shade on this plugin's pane |
| | `terminal_request` (1.1) | `{command, width, height}`: open an embedded terminal (sizes in pixels, 80–8192; clamped to the monitor). `command` uses POSIX quoting and runs without a shell (`shade::splitCommand`). One terminal per plugin and window: a new request replaces it. `signal.window` defaults to the window the pointer was last on |
| | `terminal_close` (1.1) | retract this plugin's terminal(s) |
| shade → plugin | `terminal_closed` (1.1) | `{reason, exit_code?, error?}`: `exited`, `collapsed`, `deactivated`, `closed`, or `rejected` / `failed` for a request that never opened |
| plugin ↔ plugin | anything | destination is a plugin name, or `*` to broadcast |

The `source` field is stamped by the bus, so a plugin cannot forge another
plugin's name. Delivery is queued, so `send()` never blocks or re-enters.

`shade::PaneWidget` is an ordinary `QWidget` with a few extras:
`peekSizeHint()` and `paintPeek()` draw the compact form (`morph` goes 0 → 1
while it expands), `peekPointerEvent()` handles clicks on a peek,
`notifySizeChanged()` tells the host the pane's natural size changed (the
pill springs to it), `cornerRadius()` / `roundedRect()` round a full-bleed
background, and `requestPeekRepaint()` repaints an animating peek.
`shade::MorphIcon`, `IconWidget` and `IconButton` give you morphing SVG
icons; see `include/shade/icon.h` for the supported SVG subset and the
`data-anchor` / `data-color` attributes. The palette is in
`include/shade/theme.h`.

While the shade animates, the window composites snapshots of the panes. At
rest, the selected pane is the live widget itself, so normal Qt input,
hover and child widgets just work.

## Tests

```sh
meson test -C build                   # everything
meson test -C build --suite unit      # model/pager/springs, bus, config, icons, pid/plugin scan, plugin internals
meson test -C build --suite integration
```

The integration suite (`tests/integration/run.sh`) runs entirely headless. It
starts a private D-Bus session with mock UPower, iwd, BlueZ and MPRIS
services (`mocks.py`), a fake `pactl`, and real `notify-send`. It then
drives a real shade on Qt's `offscreen` platform through `shadectl` and
checks peeks, expansion policies, player control, notification protocol
behaviour (ActionInvoked, close reasons, a single daemon), do-not-disturb
and clean shutdown. Run it with `KEEP=1` to keep its runtime directory.
