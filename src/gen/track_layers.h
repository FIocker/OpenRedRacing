// The circuit painted IN LAYERS - the user's method (2026-10-04): the theme's base ground
// everywhere, then the TRACK drawn along the centre line in the donor's road material (water on
// WATER), then its BANKS, the bands that join the track to the base. Lakes and relief come later,
// on top of this.
//
// What each layer is made of is learned from the donor, across its own road (nothing typed in):
//   - the TRACK material: the most common at the road's middle (dirt on DIRT, water on WATER);
//   - its WIDTH: the run of track vertices across the road, median and spread (DIRT2 ~2 560
//     units, WATER2's river ~1 000) - the generator varies it along the lap within that spread;
//   - the BASE: the most common ground of the map that is neither water nor the track;
//   - the BANKS: the most common sequence of grounds met going out from the track's edge until
//     the base, each with its median width. None on DIRT (90 %: dirt straight into dark grass,
//     the transition tiles ARE the bank), one or two bands on SNOW, a band of sand on WATER.
//
// The vertices are painted, then repaired (two grounds with no tile between them get the one in
// between) and the autotiler lays the tiles.
//
// Integer-only and deterministic, like the rest of the generator.
//
#pragma once

#include "gen/autotile.h"
#include "gen/track_generator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bigred::gen {

// The donor's LAKE: its largest body of water that neither reaches the map's border (the sea) nor
// lies on the road (a river). DIRT1, DIRT2, DIRT3 have one, ringed with the track's own dirt on
// 99-100 % of its shore (CONFIRMED, 2026-10-04); SNOW2 a tiny one, RIG4 four.
struct LakeLook {
    int water = -1;                // its ground
    int shore = -1;                // the ground round it (DIRT: the track's dirt)
    int shore_rings = 1;           // how many vertices of shore before the land
    int32_t area = 0;              // vertices of water
    int elongation = 1000;         // long axis over short axis, per mille
    int32_t distance = 0;          // from the road's middle to its nearest water, world units
    bool valid() const { return water >= 0 && shore >= 0 && area >= 20; }
};

struct TrackLook {
    int base = -1;                 // the base ground, everywhere the track and banks are not
    int track = -1;                // the track's ground
    bool wet = false;              // the track is water (WATER, CHOPPER)
    // Widths of the track, world units: the median run of track vertices across the road, and
    // the first and third quartiles - the range a generated lap widens and narrows within.
    int32_t width = 0, width_lo = 0, width_hi = 0;
    struct Band {
        int material;
        int32_t width;             // world units
    };
    std::vector<Band> banks;       // from the track's edge outwards, the base excluded
    int bank_share = 0;            // per mille of the road's sides that show this sequence
    LakeLook lake;
    std::string report;
    bool valid() const { return base >= 0 && track >= 0 && width > 0; }
    int32_t banks_width() const {
        int32_t w = 0;
        for (const Band& b : banks) w += b.width;
        return w;
    }
};

// Learns it from a level: its autotiler grounds `at`, its MAP.MPC and MAP.MPH, and its road's
// centre line every few hundred units with the direction of travel at each point (BAM16).
// ⚠️ A ground of SLOPES (median step of 12 height bytes or more: WATER's rocks, CHOPPER2's canyon
// walls) is never a bank: the layers are flat, and CHOPPER2's 3 000 units of rock round its river
// made a grey band on the grass.
TrackLook learn_track_look(const AutoTiles& at, const Blob& mpc, const Blob& mph, const std::vector<Point>& road,
                           const std::vector<int32_t>& heading);

// Paints the vertex grounds `V` ((map+1)^2, row by row) of a 256x256 map: the base; the LAKE if
// the donor has one (`lake_seed` places and shapes it: at the donor's distance from the track,
// the donor's size and elongation, ringed with its shore); the track along the closed centre
// line `centre` with its width at each point `width` (world units), the banks beyond its edge.
// `locked` receives the vertices of the track and of the lake's water (what the repair must
// keep). Returns a line for the report.
// `soft` (if not null) receives, near the track, how much each vertex belongs to each ground
// (0..1000, half way exactly on the track's or a bank's true edge; kSoftUnknown elsewhere), for
// lay_autotiles to choose the edge variants by the true edge rather than by a blur.
std::string paint_layers(const TrackLook& look, const std::vector<Point>& centre, const std::vector<int32_t>& width,
                         uint64_t lake_seed, std::vector<int8_t>& V, std::vector<uint8_t>& locked,
                         std::vector<std::vector<uint16_t>>* soft = nullptr);

} // namespace bigred::gen
