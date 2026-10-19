// The level backdrop: what the original paints behind the world before drawing anything else.
//
//
// Short version: the original fills the colour buffer with a stack of ANGULAR bands, each one
// a palette index at a given elevation away from the horizon. The two tables below are copied
// verbatim from the original's initialised data and are never
// written to at runtime - only the draw call ever reads them. So the tables are the
// same on every level, and the whole visual difference between a blue CHOPPER sky, an ochre MOON
// one and a teal WATER one comes from the level's own MAP.COL.
//
// Angles are stored as 16-bit BAM in the executable (65536 = one full turn); they are converted
// to degrees here because nothing else in this renderer works in BAM.
#pragma once

#include "../formats/threedw.h" // RGB

#include <array>
#include <cstddef>
#include <vector>

namespace bigred {

struct SkyBand {
    float elevation_deg; // away from the horizon; entries run from the widest angle down to 0
    int palette_index;
};

// Above the horizon, outermost first. Note how tightly the angles bunch up near 0: the original
// spends half its bands in the last 3 degrees, which is what gives the horizon its haze.
constexpr std::array<SkyBand, 16> kSkyBands{{
    {38.45f, 239}, {19.91f, 238}, {14.42f, 237}, {11.12f, 236},
    { 8.65f, 235}, { 6.73f, 234}, { 5.08f, 233}, { 3.71f, 232},
    { 2.61f, 231}, { 1.79f, 230}, { 1.24f, 229}, { 0.82f, 228},
    { 0.55f, 227}, { 0.27f, 226}, { 0.14f, 225}, { 0.00f, 224},
}};

// Below the horizon. Only two, because terrain covers nearly all of it anyway.
constexpr std::array<SkyBand, 2> kGroundBands{{
    {7.14f, 222}, {0.00f, 223},
}};

// Resolves a band's palette index against a level's MAP.COL. Out-of-range indices fall back to
// mid grey rather than black, so a malformed palette is visible as wrong rather than as "the sky
// failed to draw at all".
inline void sky_band_rgb(const std::vector<RGB>& palette, int index, float out_rgb[3]) {
    if (index >= 0 && static_cast<size_t>(index) < palette.size()) {
        out_rgb[0] = palette[static_cast<size_t>(index)].r / 255.0f;
        out_rgb[1] = palette[static_cast<size_t>(index)].g / 255.0f;
        out_rgb[2] = palette[static_cast<size_t>(index)].b / 255.0f;
        return;
    }
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.5f;
}

} // namespace bigred
