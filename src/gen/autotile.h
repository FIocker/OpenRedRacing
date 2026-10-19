// The AUTOTILER: which ground tile goes in each cell, given only the MATERIAL at each vertex of
// the grid - what a circuit generator produces, and what a level editor's brush paints.
//
// The rules are those of the shipped maps, learned from one of them (nothing typed in by hand)
// and checked by replaying that map on itself: keep only its vertex materials, lay the tiles
// again, count the cells that come back identical (DIRT2: 99.8 %).
//
//   - a tile has a material at each corner; transition tiles are learned WITHOUT colour, from
//     the fact that the four cells round a vertex agree on it;
//   - four equal corners: that material's plain tile - for DIRT2's road dirt, the one whose tyre
//     tracks follow the strip (8 tiles, one per 22.5 degrees), across it by the water;
//   - one tile for these corners: it;
//   - several (straight edge, 2:1 diagonal, pieces of a curve): the one whose picture best matches
//     the boundary smoothed over about two cells, with a per-material offset (DIRT2's dirt edge
//     hugs the dirt vertices: the grass lip covers the whole transition cell);
//   - a one-vertex ring of mud round water takes the ROCKY tiles, a wider band the smooth ones;
//   - none in the atlas (DIRT2 has no "three medium grass, one dark" tile): the tile with the
//     fewest corners wrong, the most used first - the plain medium grass, as the painter did.
//
//   - a plain ground cut from one picture (MOON, RIG, CITY4): the tile its map row and column
//     select, as the source painted it.
//
// Every shipped circuit comes back 92 % equivalent or better (the right ground, any of its plain
// variants: CHOPPER sows its grass and rock variants at random). The rules are learned per
// source level, so each theme gets its own.
//
// Integer-only and deterministic: the tiles fix the surface type under the wheels.
//
#pragma once

#include "formats/game_files.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bigred::gen {

struct AutoTiles {
    static constexpr int kMaxMaterials = 16;
    static constexpr int kPic = 8;           // a tile's material picture, kPic x kPic
    int materials = 0;
    int water = -1;                          // the water's material (the lowest), -1 if none
    std::array<bool, kMaxMaterials> wet{};   // every water ground (WATER3's river has two shades)
    std::array<std::array<int, 3>, kMaxMaterials> colour{};
    // Per MAP.MPC code: its material at each corner - (x0,z0) (x1,z0) (x0,z1) (x1,z1) - or -1.
    std::array<std::array<int8_t, 4>, 256> corners{};
    std::array<int, 256> count{};            // cells of the source using the code
    std::array<bool, 256> tile{};            // the code draws a tile (not a flat colour, not a hole)
    std::array<std::array<uint8_t, kPic * kPic>, 256> picture{};
    std::array<std::array<bool, 4>, 256> ring{};   // corner on the water's one-vertex ring
    std::array<int8_t, 256> angle{};         // a plain code's strip direction, 0..7 (x 22.5 deg), -1
    std::array<uint8_t, kMaxMaterials> fill{};     // a material's most used plain code
    std::array<bool, kMaxMaterials> oriented{};    // its plain tiles follow the strip
    std::array<int, kMaxMaterials> bias{};   // boundary offset, in field units (kField = 1.0)
    std::array<int8_t, 256> plain_of{};      // the material a plain code paints, -1 for others
    // A MOTIF: a plain ground painted with a picture cut in tiles, the code a function of the map
    // row's class and the map column's class (MOON2, RIG1, RIG4, CITY4: 2x2 by parity; MOON1:
    // 4 row classes along z, 3 column classes drawn per map column).
    struct Motif {
        std::array<int16_t, 256> row{}, col{};   // class of each map row / column, -1
        int rows = 0, cols = 0;
        std::vector<int16_t> code;                // rows x cols, -1 where never seen
    };
    std::array<Motif, kMaxMaterials> motif{};  // empty (rows == 0) for most grounds
    int round_trip_permille = 0;             // the source replayed on itself: identical cells
    int equivalent_permille = 0;             // ... or the right ground in another plain variant
    // The edge variants right on the round trip (DIRT2 1 012 / 1 015; DIRT3 75 %): whether the
    // blurred boundary explains how the source chooses them.
    int variant_permille = 0;
    std::string report;
    // Which grounds meet inside a tile of the source (a tile of exactly those two, used 10 times):
    // DIRT2 chains dirt - dark grass - medium grass - light grass, and water - dirt - dark grass.
    std::array<std::array<bool, kMaxMaterials>, kMaxMaterials> touch{};
    bool valid() const { return materials > 0; }
};

// Learns the tiles of a level archive (MAP.MPC, MAP.CHT, MAP.COL, the atlases its script's
// `texes` names) and replays it on itself to fit the boundary offsets and measure the round trip.
AutoTiles learn_autotiles(const Archive& level, const std::string& level_name);

// The material of every vertex of a map painted with these tiles: (map+1)^2 values, row by row
// (z), -1 where no tile says.
std::vector<int8_t> vertex_materials(const AutoTiles& at, const Blob& mpc);

// Lays the tiles for the vertex materials `V`. Cells whose code draws no tile (a hole, a flat
// colour) and cells marked in `keep` (if not empty) are left as they are. With `sow`, a ground
// whose plain variants the source scatters at random (CHOPPER) gets them scattered again; without,
// its most used one everywhere (the round trip). Returns the cells changed.
//
// `soft` (optional, one vector per material, (map+1)^2 values 0..1000, kSoftUnknown where not
// known): how much each vertex belongs to each ground, where the painting KNOWS where a boundary
// runs between vertices - a generated track's edge is at a known distance from its centre line.
// There it replaces the blur of the vertex grounds from which the edge variants are chosen:
// DIRT3's single track corner is a big square notch (85) or a small one (87, 83) depending on
// where the edge crosses the cell, and the blur always said "big" (user, 2026-10-04: "DIRT 1
// reste très carré au niveau des berges").
constexpr uint16_t kSoftUnknown = 0xFFFF;
int lay_autotiles(const AutoTiles& at, const std::vector<int8_t>& V, Blob& mpc,
                  const std::vector<uint8_t>& keep = {}, bool sow = true,
                  const std::vector<std::vector<uint16_t>>* soft = nullptr);

// Makes a painted vertex map layable, as the source's painter did: two grounds with no tile
// between them never share a cell, so a vertex next to one it cannot touch takes the ground that
// follows on the shortest chain of transitions, and the ring spreads outwards until every cell
// can be tiled. Vertices marked in `locked` (what the brush just painted) never change: light
// grass painted into dark grass gets its band of medium grass, water its ring of mud. The band
// is one vertex wide, the least the source ever has; wider shading is the painter's own.
// Without `locked`, the first vertex of each clash holds. Returns the vertices changed.
int repair_materials(const AutoTiles& at, std::vector<int8_t>& V, const std::vector<uint8_t>& locked = {});

// Thickens the water's one-vertex ring where the atlas has no ringed tile for a cell: DIRT2's
// paved set lacks the edge with the dirt north and the grass south (and every corner with three
// dirt and one grass), and its painter shaped the lake so as never to need them. A generated lake
// does need them; there the cell's other corners take the ring's ground, so that its outer edge
// is no longer on the ring and takes the smooth dirt tiles, which exist in every direction.
// Vertices marked in `locked` never change. Returns the vertices changed.
int thicken_rings(const AutoTiles& at, std::vector<int8_t>& V, const std::vector<uint8_t>& locked = {});

} // namespace bigred::gen
