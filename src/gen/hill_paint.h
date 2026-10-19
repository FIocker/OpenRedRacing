// The HILLS' PAINT: which ground covers the generator's own hills, learned from the donor.
//
// On every shipped map the plain is one ground (DIRT2's dark grass) and the hills wear others
// (its medium and light grass): a hill ground is ALWAYS raised, the plain's ground is the flat
// land. Which hill ground goes where is a tendency, not a rule - the lighter on the tops, the
// medium on the flanks - so the generator ranks its raised vertices by height and hands them out
// in the donor's own proportions, lowest ground first. The bands between grounds that have no
// tile in common are then repaired (repair_materials), as a painter's brush would.
//
// Measured on the shipped maps: this ranking gives the right ground to 69 % of DIRT2's raised
// vertices, 75 % of SNOW3's; the rest is the painter's hand.
//
// Integer-only and deterministic, like the rest of the generator.
//
#pragma once

#include "gen/autotile.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bigred::gen {

struct HillPaint {
    int base = -1;                 // the plain's ground
    std::vector<int> order;        // the grounds of the raised land, lowest first
    std::vector<int> share;        // ... their share of it, per mille (sums to 1000)
    std::array<bool, AutoTiles::kMaxMaterials> paintable{};   // the base and the hill grounds
    int base_height = 0;           // the plain's height byte
    std::string report;
    bool valid() const { return base >= 0 && !order.empty(); }
};

// Learns it from a level's files: the autotiler's grounds `at` (learned on the same level), its
// MAP.MPH and MAP.MPC, the code under its road (whose ground is not a hill's) and the plain's
// height byte (DonorInfo::base_height).
HillPaint learn_hill_paint(const AutoTiles& at, const Blob& mph, const Blob& mpc, uint8_t road_mpc, int base_height);

// Paints the vertex grounds `V` ((map+1)^2, as vertex_materials gives them) of a map of height
// `mph`: every vertex whose ground is a paintable one, and not marked in `keep`, takes the plain's
// ground if it is flat, a hill ground by its height's rank if it is raised. `top` (if not null)
// receives the vertices given the highest ground - the ones to hold while the bands are repaired.
// Returns the vertices changed.
int paint_hills(const HillPaint& hp, std::vector<int8_t>& V, const Blob& mph, const std::vector<uint8_t>& keep,
                std::vector<uint8_t>* top = nullptr);

} // namespace bigred::gen
