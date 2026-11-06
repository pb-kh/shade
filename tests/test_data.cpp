#include "test_data.h"

const char *const kIcon1 = R"svg(<svg viewBox="0 0 16 16">
            <path id="p" d="M1 1 h4 v4 H1 z m8 0 l4 0 0 4 -4 0 Z" />
            <path id="c" d="M2 10 C2 12 4 14 6 14 S10 12 10 10 Q12 8 14 10 T14 14 A2 2 0 0 1 10 14"
                  fill="none" stroke="#000" stroke-width="1.5" stroke-linecap="round"/>
        </svg>)svg";

const char *const kIcon2 = R"svg(<svg width="20" height="10">
            <g transform="translate(10 0)" opacity="0.5" fill="none" stroke="black">
              <circle id="dot" cx="2" cy="5" r="2"/>
              <rect id="r" x="0" y="0" width="4" height="4" rx="1" data-color="red"/>
            </g>
            <line id="l" x1="0" y1="0" x2="10" y2="10"/>
            <polygon id="tri" points="0,0 4,0 2,3" data-anchor="2 1"/>
            <metadata><path d="M0 0 L 1 1"/></metadata>
        </svg>)svg";

const char *const kIcon3 = R"svg(<svg viewBox="0 0 16 16"><path id="s" d="M0 8 L16 8" stroke="#000" fill="none"/></svg>)svg";

const char *const kIcon4 = R"svg(<svg viewBox="0 0 16 16"><path id="s" d="M0 0 L16 0" stroke="#000" fill="none"/>
            <circle id="dot" cx="8" cy="12" r="2"/></svg>)svg";

const char *const kIcon5 = R"svg(<svg viewBox="0 0 16 16"><path id="q" d="M2 2 H14 V14 H2 Z"/></svg>)svg";

const char *const kIcon6 = R"svg(<svg viewBox="0 0 16 16"><path id="q" d="M14 14 H2 V2 H14 Z"/></svg>)svg";

const char *const kConfig1 = R"(
[shade]
bar_width = 240
peek_timeout_ms = 3000
corner_radius = 12.5
terminal = "foot"
disabled_plugins = ["media"]

[theme]
bg = "#101010"
accent = "#ff8800"

["plugin:stats"]
network_backend = "iwd"
battery_low = [20, 10, 5]
[ "plugin:stats".peeks ]
volume = false
)";

const char *const kConfig2 = R"(
[shade]
bar_width = "wide"
idle_timeout_ms = 1.5
mystery = 1
[theme]
bg = "not-a-color"
)";

