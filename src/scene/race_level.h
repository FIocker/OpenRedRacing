// Everything a RaceSession needs out of a level, and the FIELD it races with - built the same
// way by the game, the network game and the benches.
//
// ⚠️ WHY THIS EXISTS: until 2026-09-25 the race screen and `physics_bench` each built the
// terrain, the ring and the ghosts with their own copy of the same forty lines. The network
// game needs every machine to build EXACTLY the same session from the same START message; a
// third copy is how two of them would one day disagree by a line. One function, and every
// caller gets the same race.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "formats/game_files.h"
#include "formats/level_script.h"
#include "sim/race.h"
#include "sim/race_session.h"
#include "sim/replay.h"
#include "sim/vehicle.h"

namespace bigred {

struct LevelScene;

// The simulation's view of a loaded level. The terrain points into `scene`'s buffers, so the
// scene must outlive it.
struct RaceLevel {
    std::string level_txt;
    std::vector<std::string> cars;    // the level's own `car` list
    SimTerrain terrain;
    std::vector<Gate> gates;
    std::vector<CheckLine> lines;
    StartingGrid grid;
    NitroReserve nitros;
    RespawnPoint origin;              // where a level without a `grid` line starts everyone
    int laps = 3;
    int countdown_ticks = 0;
    ReplayRates rates;
};

// `game_dir` loads the level's first vehicle to check the network game's extra grid places
// with (sim/race.h, extend_grid); empty skips that check.
void build_race_level(const LevelScene& scene, const std::string& level, RaceLevel& out,
                      const std::string& game_dir = std::string());

// One human racer to put on the grid.
struct FieldHuman {
    int grid = 0;                     // the grid block, 0..11
    std::string vehicle;              // empty = the level's first `car`
    int damage_option = VehicleSim::kDefaultDamageOption;
};

// The racers of one race, in SESSION order - which is GRID order: session racer i stands on
// grid block i. Sized once and never resized: the RacerSetups point into the vectors.
struct RaceField {
    std::vector<VehicleTuning> tunings;
    std::vector<ArchivePtr> vhc;                              // the models' archives, never null
    std::vector<std::string> vehicle;                         // the name, per racer
    std::vector<ReplayTrack> tracks;
    std::vector<RacerSetup> racers;
    std::vector<int> human;           // session racer -> index into `humans`, -1 = a ghost
    std::vector<int> session_of;      // index into `humans` -> session racer
    std::vector<int> colour;          // 0..5, the mini-map dot
    int ghosts = 0;
};

// The humans at their blocks and, with `ghosts`, the level's recorded ghost `N.REC` on every
// other block N (as in the original, the ghost of a block a human takes is not raced). When no ghost
// could be loaded - or none was asked for - the humans close up onto blocks 0..n-1 in grid
// order: alone, the player takes the first block. False when a human's vehicle cannot load.
bool build_race_field(const std::string& game_dir, const LevelScene& scene, const RaceLevel& level,
                      const std::vector<FieldHuman>& humans, bool ghosts, RaceField& out);

} // namespace bigred
