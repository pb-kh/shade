#pragma once

#include <array>
#include <QtGlobal>

// Central place for the widget's layout and animation tuning. Everything here
// is constexpr so the look and feel can be changed without touching the logic.
namespace shade::config
{

struct SectionSize
{
    int width;
    int height;
    const char *label;
};

// The five sections, shown one at a time. The first section is selected at
// startup. Widths and heights are in logical pixels.
inline constexpr std::array<SectionSize, 5> kSections = {{
    {100, 30, "section 1"},
    {150, 40, "section 2"},
    {200, 50, "section 3"},
    {250, 60, "section 4"},
    {300, 70, "section 5"},
}};

inline constexpr int kSectionCount = static_cast<int>(kSections.size());

// Horizontal space between two section viewports on the scroll track. The
// labels travel across this track while the view is being scrolled.
inline constexpr qreal kSectionGap = 36.0;

// Center of each section on the horizontal scroll track. Section 0 sits at 0;
// the rest are laid out so consecutive labels are separated by half of each
// width plus kSectionGap.
constexpr std::array<qreal, kSectionCount> makeSectionCenters()
{
    std::array<qreal, kSectionCount> centers{};
    qreal center = 0.0;
    for (int i = 1; i < kSectionCount; ++i) {
        center += (kSections[i - 1].width + kSections[i].width) / 2.0 + kSectionGap;
        centers[i] = center;
    }
    return centers;
}

inline constexpr std::array<qreal, kSectionCount> kSectionCenters = makeSectionCenters();

// Resting state: the thin bar at the top of the screen.
inline constexpr int kCollapsedWidth = 300;
inline constexpr int kCollapsedHeight = 1;

// Distance between the top of the screen and the expanded pill.
inline constexpr int kPillTopOffset = 10;

// How much of the top edge reacts to the pointer while collapsed.
inline constexpr int kHoverZoneHeight = 16;

// The transparent layer surface is larger than every section so size changes
// never require a Wayland surface resize. Keep enough room for the bounce
// overshoot; the values below are derived from the section table.
inline constexpr qreal kOvershootAllowance = 1.25;
inline constexpr int kCanvasPadding = 24;

constexpr int largestSectionWidth()
{
    int largest = 0;
    for (const SectionSize &section : kSections) {
        if (section.width > largest) {
            largest = section.width;
        }
    }
    return largest;
}

constexpr int largestSectionHeight()
{
    int largest = 0;
    for (const SectionSize &section : kSections) {
        if (section.height > largest) {
            largest = section.height;
        }
    }
    return largest;
}

inline constexpr int kCanvasWidth =
    static_cast<int>(largestSectionWidth() * kOvershootAllowance) + kCanvasPadding;
inline constexpr int kCanvasHeight =
    kPillTopOffset + static_cast<int>(largestSectionHeight() * kOvershootAllowance) + kCanvasPadding;

// --- Hover expansion -------------------------------------------------------
inline constexpr int kExpandDurationMs = 340;
inline constexpr int kCollapseDurationMs = 260;
inline constexpr qreal kExpandBounceAmplitude = 1.35;

// --- Sticky scroll ---------------------------------------------------------
// The track does not follow the fingers 1:1 at first. For the first
// kStickyPullDistance pixels of finger travel it only moves by
// kStickyResistance of that distance, then it breaks free and follows 1:1.
// This is the "gum stuck to a table" feel: a little hard to pull, then free.
inline constexpr qreal kStickyPullDistance = 28.0;
inline constexpr qreal kStickyResistance = 0.22;
static_assert(kStickyPullDistance > 0.0);
static_assert(kStickyResistance > 0.0 && kStickyResistance <= 1.0);

// After the fingers stop, wait this long before deciding the gesture has
// ended. Keep this short so the snap starts without a visible pause; if the
// compositor reports Qt::ScrollEnd, the gesture ends immediately.
inline constexpr int kScrollReleaseDelayMs = 50;

// --- Paging ----------------------------------------------------------------
// One gesture moves at most one section. Pulling past that rubber-bands by at
// most kOverscrollMaxPx (so the section after the next only peeks in a little)
// and then bounces back on release.
inline constexpr qreal kOverscrollMaxPx = 36.0;
inline constexpr qreal kOverscrollResistance = 0.6;

// On release the pill advances if it was pulled this fraction of the way to the
// neighbour, or flicked faster than kFlickMinSpeedPxPerSec.
inline constexpr qreal kAdvanceFraction = 0.25;
inline constexpr qreal kFlickMinSpeedPxPerSec = 80.0;
inline constexpr qreal kReleaseVelocitySmoothing = 0.55;
static_assert(kOverscrollMaxPx > 0.0 && kOverscrollResistance > 0.0);
static_assert(kAdvanceFraction > 0.0 && kAdvanceFraction < 1.0);
static_assert(kReleaseVelocitySmoothing > 0.0 && kReleaseVelocitySmoothing <= 1.0);

// --- Snap animation --------------------------------------------------------
// A slow release uses the long duration and gentle bounce; a fast release uses
// the short duration and stronger bounce. Speeds between the two are
// interpolated.
inline constexpr int kSnapDurationMs = 340;
inline constexpr int kSnapFastDurationMs = 220;
inline constexpr qreal kSnapBounceAmplitude = 1.3;
inline constexpr qreal kSnapFastBounceAmplitude = 2.0;

// Release speed (logical pixels per second) that maps to the fastest snap.
inline constexpr qreal kMaxScrollSpeedPxPerSec = 4000.0;

// Wheel deltas are converted to pixels with this factor when the platform only
// provides angle deltas (120 units == one wheel notch).
inline constexpr qreal kPixelsPerWheelNotch = 40.0;

// A positive horizontal delta advances to the next section. Flip to -1.0 if
// your touchpad/compositor reports the opposite direction.
inline constexpr qreal kScrollDirectionSign = 1.0;

// --- Label -----------------------------------------------------------------
inline constexpr qreal kTextHeightRatio = 0.45;
inline constexpr int kTextMinPixelSize = 12;
inline constexpr int kTextMaxPixelSize = 22;

// The label starts fading in once the pill has travelled this fraction of the
// way down from the screen edge.
inline constexpr qreal kTextFadeStart = 0.25;

} // namespace shade::config
