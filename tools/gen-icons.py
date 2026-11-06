#!/usr/bin/env python3
"""Generates the authored state icons for the bundled plugins.

Icons are plain SVG. Contours that share an `id` across files morph into each
other when a plugin switches between them (see include/shade/icon.h); contours
present in only one state grow from / shrink into `data-anchor`.

Media icons reuse the artwork of ../shade's media-controls plugin; play/pause,
shuffle and loop are re-authored as matching contour sets (geometry from
shade's media-icon-morph.c).
"""
import math
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SW = 'stroke-width="1.7" fill="none" stroke="#000"'


def svg(*elements, view="0 0 16 16"):
    body = "\n  ".join(elements)
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view}">\n  {body}\n</svg>\n'


def write(plugin, name, content):
    d = os.path.join(ROOT, "plugins", plugin, "icons")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".svg"), "w") as f:
        f.write(content)


def path(id_, d, extra=""):
    return f'<path id="{id_}" d="{d}" {extra}/>'


# --------------------------------------------------------------------------
# media
# --------------------------------------------------------------------------

def media():
    src = os.path.join(ROOT, "..", "shade", "plugins", "media-controls", "resources", "icons")
    for name in ("media-skip-backward", "media-skip-forward"):
        p = os.path.join(src, name + ".svg")
        if os.path.exists(p):
            shutil.copy(p, os.path.join(ROOT, "plugins", "media", "icons", name + ".svg"))

    # Pause: two rounded bars (shade's media-playback-pause geometry).
    bar = lambda x0, x1: (f"M {x0} 3.5 L {x0} 12.5 C {x0} 13.33 {x0 + 0.67} 14 {(x0 + x1) / 2} 14 "
                          f"C {x1 - 0.67} 14 {x1} 13.33 {x1} 12.5 L {x1} 3.5 C {x1} 2.67 {x1 - 0.67} 2 {(x0 + x1) / 2} 2 "
                          f"C {x0 + 0.67} 2 {x0} 2.67 {x0} 3.5 Z")
    write("media", "pause", svg(path("left", bar(3, 6), 'fill="#000"'), path("right", bar(10, 13), 'fill="#000"')))
    # Play: shade's rounded triangle, split into a left and a right half so
    # each half morphs into one pause bar.
    write("media", "play", svg(
        path("left", "M 4 4 L 4 12 C 4 12.8 4.9 13.4 5.7 12.97 L 8.4 11.35 L 8.4 4.65 L 5.7 3.03 C 4.9 2.6 4 3.2 4 4 Z", 'fill="#000"'),
        path("right", "M 8.2 4.53 L 8.2 11.47 L 12.43 8.94 C 13.15 8.5 13.15 7.5 12.43 7.06 Z", 'fill="#000"'),
    ))

    arrow = 'stroke-linecap="round" stroke-linejoin="round"'
    # Shuffle off ("consecutive": two parallel routes) / on (crossed routes).
    write("media", "shuffle-off", svg(
        path("arrow-low", "M 13 10 L 15 12 L 13 14", f"{SW} {arrow}"),
        path("arrow-up", "M 13 2 L 15 4 L 13 6", f"{SW} {arrow}"),
        path("route-low", "M 1 12 L 15 12", f'{SW} stroke-linecap="butt"'),
        path("route-up", "M 1 4 L 15 4", f'{SW} stroke-linecap="butt"'),
    ))
    write("media", "shuffle-on", svg(
        path("arrow-up", "M 13 6 L 15 4 L 13 2", f"{SW} {arrow}"),
        path("arrow-low", "M 13 14 L 15 12 L 13 10", f"{SW} {arrow}"),
        path("route-up", "M 15 4 L 9.378 4 C 8.533 4 7.761 4.477 7.384 5.233 L 4.616 10.767 C 4.239 11.523 3.467 12 2.622 12 L 0 12", f'{SW} stroke-linecap="butt"'),
        path("route-low", "M 15 12 L 9.378 12 C 8.533 12 7.761 11.523 7.384 10.767 L 4.616 5.233 C 4.239 4.477 3.467 4 2.622 4 L 0 4", f'{SW} stroke-linecap="butt"'),
    ))

    # Loop status: none / playlist / track (eight semantic contours).
    def loop(state):
        track = state == "track"
        none = state == "none"
        tail = 8 if track else 9
        joint = 10 if track else 11
        els = [
            path("top-arrow", f"M {tail} 2 L {joint} 4 L {tail} 6", f"{SW} {arrow}"),
            path("top-stem", f"M {joint} 4 L 4 4", f"{SW} {arrow}"),
            path("top-return",
                 ("M 1.663 5.118 C 1.248 5.633 1 6.287 1 7" if none else "M 4 4 C 2.343 4 1 5.343 1 7")
                 + " L 1 9 C 1 9.756 1.279 10.446 1.741 10.974", f"{SW} {arrow}"),
            path("bottom-arrow", "M 7 14 L 5 12 L 7 10", f"{SW} {arrow}"),
            path("bottom-stem", f"M 5 12 L {8.848 if none else 12} 12", f"{SW} {arrow}"),
            path("bottom-return",
                 "M 12 12 C 13.657 12 15 10.657 15 9 " + ("L 15 8" if track else "L 15 7 C 15 6.244 14.721 5.554 14.259 5.026"),
                 f"{SW} {arrow}"),
        ]
        if track:
            els.append('<circle id="track-dot" cx="14" cy="4" r="1" fill="#000" data-anchor="14 4"/>')
        if none:
            els.append(path("slash", "M 1 1 L 15 15", f'{SW} {arrow} data-anchor="8 8"'))
        return svg(*els)

    for st in ("none", "playlist", "track"):
        write("media", "loop-" + st, loop(st))

    # Placeholder art (music note) for the no-art state.
    write("media", "note", svg(
        path("stem", "M 6 12 L 6 3 L 13 1.5 L 13 10.5", f"{SW} {arrow}"),
        '<circle id="head-a" cx="4.5" cy="12.5" r="2" fill="#000"/>',
        '<circle id="head-b" cx="11.5" cy="10.5" r="2" fill="#000"/>',
    ))


# --------------------------------------------------------------------------
# stats
# --------------------------------------------------------------------------

def stats():
    rnd = 'stroke-linecap="round" stroke-linejoin="round"'

    # Wi-Fi: a dot and three arcs; signal strength lights arcs up. Offline
    # adds a slash, the arcs fade.
    def arc(r):
        a0, a1 = math.radians(225), math.radians(315)
        cx, cy = 8, 13.5
        x0, y0 = cx + r * math.cos(a0), cy + r * math.sin(a0)
        x1, y1 = cx + r * math.cos(a1), cy + r * math.sin(a1)
        return f"M {x0:.3f} {y0:.3f} A {r} {r} 0 0 1 {x1:.3f} {y1:.3f}"

    def wifi(level, offline=False):
        els = ['<circle id="dot" cx="8" cy="13" r="1.25" fill="#000" ' + ('opacity="0.35"' if offline else '') + '/>']
        for i, r in enumerate((4.2, 7.6, 11.0), start=1):
            op = 1.0 if (not offline and level >= i + 1) else 0.25
            els.append(path(f"arc{i}", arc(r), f'{SW} {rnd} opacity="{op}"'))
        if offline:
            els.append(path("slash", "M 2 2 L 14 14", f'{SW} {rnd} data-anchor="8 8"'))
        return svg(*els)

    for lvl in range(5):
        write("stats", f"wifi-{lvl}", wifi(lvl))
    write("stats", "network-offline", wifi(0, offline=True))
    # Wired: three boxes connected by lines (an ethernet port tree).
    write("stats", "network-wired", svg(
        '<rect id="top" x="6" y="1.5" width="4" height="3.5" rx="0.8" fill="#000"/>',
        '<rect id="left" x="1.5" y="11" width="4" height="3.5" rx="0.8" fill="#000"/>',
        '<rect id="right" x="10.5" y="11" width="4" height="3.5" rx="0.8" fill="#000"/>',
        path("trunk", "M 8 5 L 8 8 M 3.5 11 L 3.5 8 L 12.5 8 L 12.5 11", f"{SW} {rnd}"),
    ))

    # Bluetooth rune. Disabled: rune dims + slash. Connected: two dots.
    rune = "M 4.5 4.8 L 11.3 10.9 L 8 13.9 L 8 2.1 L 11.3 5.1 L 4.5 11.2"
    write("stats", "bluetooth-on", svg(path("rune", rune, f"{SW} {rnd}")))
    write("stats", "bluetooth-off", svg(
        path("rune", rune, f'{SW} {rnd} opacity="0.35"'),
        path("slash", "M 2 2 L 14 14", f'{SW} {rnd} data-anchor="8 8"'),
    ))
    write("stats", "bluetooth-connected", svg(
        path("rune", rune, f"{SW} {rnd}"),
        '<circle id="dot-l" cx="2" cy="8" r="1.15" fill="#000" data-anchor="4.5 8"/>',
        '<circle id="dot-r" cx="14" cy="8" r="1.15" fill="#000" data-anchor="11.5 8"/>',
    ))

    # Battery: body outline, cap, a level fill that grows with charge, and a
    # bolt while charging. 11 levels (0..100 by 10) x {discharging, charging}.
    for lvl in range(0, 11):
        for charging in (False, True):
            w = max(0.01, 9.0 * lvl / 10.0)
            low = lvl <= 1 and not charging
            fill_attr = 'fill="#000"' + (' data-color="red"' if low else "")
            els = [
                '<rect id="body" x="1" y="4.5" width="12.5" height="7" rx="2" fill="none" stroke="#000" stroke-width="1.4"/>',
                '<rect id="cap" x="14.3" y="6.5" width="1.4" height="3" rx="0.6" fill="#000"/>',
                f'<rect id="level" x="2.75" y="6.25" width="{w:.2f}" height="3.5" rx="0.8" {fill_attr} data-anchor="2.75 8"/>',
            ]
            if charging:
                els.append('<path id="bolt" d="M 8.2 2.6 L 5.4 8.4 L 7.6 8.4 L 6.8 13.4 L 9.8 7.4 L 7.6 7.4 Z" '
                           'fill="#000" stroke="#000" stroke-width="0.6" stroke-linejoin="round" data-color="yellow" data-anchor="7.6 8"/>')
            write("stats", f"battery-{lvl * 10}{'-charging' if charging else ''}", svg(*els))
    write("stats", "battery-missing", svg(
        '<rect id="body" x="1" y="4.5" width="12.5" height="7" rx="2" fill="none" stroke="#000" stroke-width="1.4" opacity="0.4"/>',
        '<rect id="cap" x="14.3" y="6.5" width="1.4" height="3" rx="0.6" fill="#000" opacity="0.4"/>',
    ))

    # Volume: speaker + three waves (lit by level); muted adds a cross.
    speaker = "M 1.5 6 L 4 6 L 7.5 2.8 L 7.5 13.2 L 4 10 L 1.5 10 Z"

    def wave(i):
        r = 2.4 + 2.3 * i
        a = math.radians(48)
        x0, y0 = 7.5 + r * math.cos(-a), 8 + r * math.sin(-a)
        x1, y1 = 7.5 + r * math.cos(a), 8 + r * math.sin(a)
        return f"M {x0:.3f} {y0:.3f} A {r} {r} 0 0 1 {x1:.3f} {y1:.3f}"

    def volume(level, muted=False):
        els = [path("speaker", speaker, 'fill="#000" stroke="#000" stroke-width="0.8" stroke-linejoin="round"')]
        if not muted:
            for i in range(3):
                op = 1.0 if level > i else 0.25
                els.append(path(f"wave{i}", wave(i), f'{SW} {rnd} opacity="{op}" data-anchor="8 8"'))
        else:
            els.append(path("x1", "M 10.5 5.5 L 15 10", f'{SW} {rnd} data-anchor="12.75 7.75"'))
            els.append(path("x2", "M 15 5.5 L 10.5 10", f'{SW} {rnd} data-anchor="12.75 7.75"'))
        return svg(*els)

    for lvl in range(4):
        write("stats", f"volume-{lvl}", volume(lvl))
    write("stats", "volume-muted", volume(0, muted=True))

    # Clock face used by the time peek.
    write("stats", "clock", svg(
        '<circle id="face" cx="8" cy="8" r="6.3" fill="none" stroke="#000" stroke-width="1.5"/>',
        path("hands", "M 8 4.5 L 8 8 L 10.5 9.5", f"{SW} {rnd}"),
    ))


# --------------------------------------------------------------------------
# notifications
# --------------------------------------------------------------------------

def notifications():
    rnd = 'stroke-linecap="round" stroke-linejoin="round"'
    bell = "M 4 11.5 L 4 7 C 4 4.7 5.8 3 8 3 C 10.2 3 12 4.7 12 7 L 12 11.5 L 13.5 12.5 L 2.5 12.5 Z"
    write("notifications", "bell", svg(
        path("bell", bell, f"{SW} {rnd}"),
        path("clapper", "M 6.6 14.4 C 7.4 15.1 8.6 15.1 9.4 14.4", f"{SW} {rnd}"),
        path("knob", "M 8 1.4 L 8 3", f"{SW} {rnd}"),
    ))
    write("notifications", "bell-off", svg(
        path("bell", bell, f'{SW} {rnd} opacity="0.4"'),
        path("clapper", "M 6.6 14.4 C 7.4 15.1 8.6 15.1 9.4 14.4", f'{SW} {rnd} opacity="0.4"'),
        path("knob", "M 8 1.4 L 8 3", f'{SW} {rnd} opacity="0.4"'),
        path("slash", "M 2 2 L 14 14", f'{SW} {rnd} data-anchor="8 8"'),
    ))
    # Generic app icon fallback: a rounded square with a dot.
    write("notifications", "app", svg(
        '<rect id="frame" x="2" y="2" width="12" height="12" rx="3.5" fill="none" stroke="#000" stroke-width="1.6"/>',
        '<circle id="dot" cx="8" cy="8" r="2" fill="#000"/>',
    ))
    write("notifications", "close", svg(
        path("a", "M 4 4 L 12 12", f"{SW} {rnd}"),
        path("b", "M 12 4 L 4 12", f"{SW} {rnd}"),
    ))
    write("notifications", "clear", svg(
        path("a", "M 3 5 L 13 5", f"{SW} {rnd}"),
        path("b", "M 5 8.5 L 11 8.5", f"{SW} {rnd}"),
        path("c", "M 7 12 L 9 12", f"{SW} {rnd}"),
    ))


if __name__ == "__main__":
    media()
    stats()
    notifications()
    print("icons written", file=sys.stderr)
