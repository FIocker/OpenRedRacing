// The ground's TILES, learned from a donor circuit and laid again on a generated map.
//
// A MAP.MPC code picks a 32x32 tile of a hand-painted atlas (formats/terrain.h). The shipped maps
// are painted CORNER BY CORNER, as a 2D tile set is: every tile has a ground MATERIAL at each of
// its four corners - water, mud, grass, sand, cliff - and a transition tile is one whose corners
// differ (DIRT1's lake: 96 is water all round, 97 has mud on its west corners and water on its
// east ones, 102..108 are the corners of the shore). So:
//
//   - the materials are read off the atlas itself, from the colour of each tile's corners;
//   - the donor's map says which tiles stand for which four corners, and how often;
//   - on the generated map every VERTEX of the grid gets a material, and every cell the tile
//     whose corners match.
//
// What decides a vertex's material is what the shipped maps obey, measured on them:
//   - the tiles the corridor copied vote (they carry the donor's layout round its road);
//   - some materials only live on steep ground (WATER1's rocks, DIRT3's terraces, MOON1's crater
//     rims: a median step of 15 to 36 height bytes, against 0 to 3 for grass, mud and water) - a
//     cliff on flat ground becomes the ground round it, a steep vertex becomes cliff;
//   - two materials with no transition tile between them never touch: the donor has shores
//     between water and MUD only, so water gets its ring of mud before the grass.
//
// Deterministic and integer-only, like the rest of the generator.
//
#pragma once

#include "formats/game_files.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bigred::gen {

struct GroundTiles {
    static constexpr int kMaxMaterials = 16;
    int materials = 0;
    std::array<std::array<int, 3>, kMaxMaterials> colour{};   // RGB 0..255, for the report
    // Each code's material at its four corners - (x0,z0) (x1,z0) (x0,z1) (x1,z1), x and z the
    // map's own - or -1 for a code with no tile (a flat colour, a hole).
    std::array<std::array<int8_t, 4>, 256> corners{};
    std::array<int, 256> count{};                             // cells of the donor using it
    // A tile of a 2x2 MOTIF - a 64x64 picture cut in four, as MOON2, RIG1, RIG4 and CITY4 paint
    // their plain ground: the parity (gx & 1) + 2 (gz & 1) it always stands on, -1 for a free tile.
    std::array<int8_t, 256> parity{};
    std::array<int64_t, kMaxMaterials> weight{};              // corners of the donor made of it
    // Which materials meet inside some tile of the donor.
    std::array<std::array<bool, kMaxMaterials>, kMaxMaterials> touch{};
    // A material of STEEP ground: the step (height bytes to a neighbour) at the first quartile
    // of its pure tiles, 0 for a material of level ground.
    std::array<int, kMaxMaterials> cliff_step{};
    int sea = -1;                    // the sea's material, if the donor has one
    int sea_level = 0;               // its height byte (the median of its pure cells)
    bool valid() const { return materials > 0; }
    bool is_water(uint8_t code) const {
        if (sea < 0) return false;
        for (int8_t m : corners[code])
            if (m == sea) return true;
        return false;
    }
    std::string report;
};

// Learns the tiles of `donor` (a level archive: MAP.MPC/MPH/CHT/COL, its script's `texes` and the
// atlases it names). `outside_code` is the code that fills the map far from the road (the sea
// round an island), or -1.
GroundTiles learn_ground_tiles(const Archive& donor, const std::string& level_name, int outside_code);

// Lays the tiles again on a 256x256 map whose relief is final. `mpc` holds the codes the
// corridor copied and is rewritten; `road` and `water` mark the cells the relief anchored, which
// only take road codes (`road_code`) and water codes respectively. Returns a line for the report.
std::string lay_ground_tiles(const GroundTiles& gt, const Blob& mph, Blob& mpc,
                             const std::vector<uint8_t>& road, const std::vector<uint8_t>& water,
                             const std::array<bool, 256>& road_code);

} // namespace bigred::gen
