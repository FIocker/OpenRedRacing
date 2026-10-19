// The circuit generator: a whole circuit from a THEME and a SEED.
//
// A theme is one of the original's families (DIRT, CITY, SNOW...). Its first circuit is the
// DONOR: the generator borrows everything it does not make - shapes, textures, palette, sky,
// sounds, the script's settings - and makes the rest: the relief, the ground, the gate ring,
// the checkpoint lines, the starting grid and the scattered scenery. The vehicles are the ones
// the family's circuits allow.
//
// The output is a set of files named as inside a `.LEV` (LevelScene::load_files reads it, and
// game/level_catalog.h's add_level will declare it to the game), so nothing downstream knows
// the circuit was generated.
//
// ⚠️ DETERMINISTIC AND INTEGER-ONLY: the same (theme, seed) gives the same bytes on every
// machine, so a network race can share a circuit by its seed alone. No float, no hash-ordered
// container in anything that reaches the files.
//
#pragma once

#include "formats/game_files.h"
#include "sim/race.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bigred::gen {

// One family of circuits: its name, the circuit the generator borrows from, and all of the
// family's circuits (whose `car` lines together say which vehicles race on it).
struct Theme {
    std::string name;
    std::string donor;
    std::vector<std::string> levels;
};
const std::vector<Theme>& themes();
// The theme called `name` (case-insensitive), or nullptr.
const Theme* find_theme(const std::string& name);

// What a donor circuit teaches the generator, MEASURED on its own files rather than assumed.
struct DonorInfo {
    // The MAP.MPC codes found most often ON the road (under the gate centres), at the road's
    // edge, and far from the track - the three the generator paints with.
    uint8_t road_mpc = 0, verge_mpc = 0, field_mpc = 0;
    int road_half_width = 1100;     // measured: where the road code stops, world units
    // LAND: the ground round the road, where the scenery stands. `field_mpc` is the OUTSIDE,
    // what fills the map far from the road: on DIRT1 the SEA round an island. `coast_distance`
    // is how far from the road the outside takes over (0: the outside IS the land, as on MOON),
    // and `outside_height` its height byte. `road_height`: the road's median height byte.
    uint8_t land_mpc = 0;
    int coast_distance = 0, outside_height = 0, road_height = 0;
    int32_t lap_length = 0;         // round the gate centres, world units
    // THE CROSS-SECTION: how the ground sits against the road. For each band of distance from
    // the road's middle (kXsecStep units wide), the median height of the ground minus the
    // road's own height there, in height bytes, and how much it varies (median absolute
    // deviation). A river on WATER is a hollow between banks; a road on DIRT sits in its land.
    static constexpr int kXsecBins = 32, kXsecStep = 500;
    int xsec_median[kXsecBins] = {}, xsec_spread[kXsecBins] = {};
    // The ground codes across the road, band by band (kBandStep units): the road, its edge, the
    // verge, as the shipped circuit paints them. The most common code in each band.
    static constexpr int kBandBins = 16, kBandStep = 256;
    uint8_t band_mpc[kBandBins] = {};
    // The map's EDGE: the mean height byte of each ring of texels at distance d from the
    // border, d = 0..kEdgeRings-1, and the median of the whole map.
    static constexpr int kEdgeRings = 24;
    int edge_mean[kEdgeRings] = {};
    int median_height = 0;
    int min_height = 0, max_height = 0;
    // THE MASSIF: the shipped maps are a flat plain at one level (`base_height`, the most common
    // height of the land) with the relief in one mass round the road. `raised_permille[b]`: the
    // share of the land more than 2 bytes above the plain at kMassifStep * b to (b + 1) units
    // from the road. `foot_rise[k]`: the median height above the plain k texels inside a hill
    // from its foot (DIRT1 5, 15, 23 at 1, 2, 3: ~8 bytes a texel), `max_excess` the highest.
    static constexpr int kMassifBins = 20, kMassifStep = 2000, kFootBins = 24;
    int base_height = 0, max_excess = 0;
    int raised_permille[kMassifBins] = {};
    int foot_rise[kFootBins] = {};
    // The CHECKP shape: its MAP.SHL slot and category byte, and how its yaw relates to the
    // gate it lies in (instance angle_y minus gate heading in BAM16, the most common value).
    int checkp_slot = -1;
    uint8_t checkp_category = 50;
    int32_t checkp_yaw_offset = 0;
    // How high a CHECKP line stands above the ground under it, world units (the median): 0 on
    // the circuits driven on the ground, 200 to 1 300 on CHOPPER - lines laid on the ground there
    // were flown over, and the lap never counted.
    int32_t checkp_lift = 0;
    // The grid's origin in gate 0's frame (along = the direction of travel, across = from the
    // road's centre, both world units) and its heading minus gate 0's.
    int32_t grid_along = 0, grid_across = 0, grid_yaw_offset = 0;
    std::string report;             // everything above, readable
};
DonorInfo analyse_donor(const Archive& lev, const std::string& level_name);

// A shipped circuit's ROAD PROFILE: the ground's height (world units, up) under the middle of its
// gate ring, every 256 units round the lap, in the direction of travel. What the generator lays
// along its own centre line, so that the rises, dips and crests are a real circuit's.
std::vector<int32_t> road_profile(const Archive& lev);

struct Point {
    int32_t x = 0, z = 0;
};

// A generated circuit.
struct Track {
    std::string theme, name;
    uint64_t seed = 0;
    Archive files;                  // named as inside a .LEV
    int map_w = 256, map_h = 256;
    std::vector<Point> centre;      // the road's centre line, every kSampleStep units
    std::vector<int32_t> width;     // the track's width at each centre point (layers only), world units
    std::vector<Gate> gates;
    std::vector<std::string> cars;  // the vehicles the circuit allows
    int32_t length = 0;             // one lap, world units
    int attempts = 0;               // centre lines drawn before one was accepted
    std::string profile_from;       // the circuit whose road profile it borrows, and where from
    DonorInfo donor;                // what the donor taught: the ground codes, for top_view
    // FNV-1a over every generated file, in name order: two machines that generated the same
    // (theme, seed) must print the same one - what sharing a circuit by its seed rests on.
    uint32_t fingerprint = 0;
    std::string report;
};

// How the ground is made.
//  - LAYERS (the user's method, 2026-10-04): flat; the theme's base ground everywhere, the track
//    drawn as a closed chain of Bezier curves in the donor's road material, its banks, then the
//    tiles (gen/track_layers.h). No scenery yet.
//  - CORRIDOR (2026-09-29 .. 10-01): the donor's landscape carried along the road, the massif,
//    the hills' paint, the scattered scenery. Kept to compare.
enum class Method { Layers, Corridor };

// Generates the circuit (theme, seed). `name` is what the files are named after
// (`<name>.TXT`). Throws std::runtime_error when the theme's donor cannot be read.
Track generate(const std::string& game_dir, const std::string& theme, uint64_t seed,
               const std::string& name = "GEN", Method method = Method::Layers);

// The circuit seen from above, one pixel per texel: relief shaded, ground codes coloured, the
// centre line, the gates and the grid drawn over it. RGB, `w * h * 3` bytes, rows from the
// map's z = 0. For looking at a circuit without a window.
std::vector<uint8_t> top_view(const Track& t);

} // namespace bigred::gen
