// What can be raced, and with what - read from the game's own files, never transcribed.
//
// ⚠️ THE ROSTER IS NEVER WRITTEN DOWN TWICE. A level's `car` lines live in its own script,
// so anything that hard-codes them is wrong the day a level is
// re-read. The launcher that came before this carried a table typed out by hand and it was
// already wrong - it offered the three RUF circuits, which cannot load at all.
//
// This is the one place that answers the question, for `--list`, for the menus, and for
// whatever asks next.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "formats/game_files.h"
#include "formats/tourn.h"

namespace bigred {

struct LevelEntry {
    std::string name;                 // "DIRT1" - the file, and what a bug report names
    // ⚠️ WHAT THE PLAYER READS: the place, out of the localised tables - "JAPAN", not
    // "DIRT1". The original shows the file name nowhere. Falls back to `name` when the tables
    // are missing, so a menu is never blank.
    std::string title;
    std::vector<std::string> cars;    // what its own script allows, FEHELI removed
    // Its music, from its line of `TOURN.TXT` (formats/tourn.h): the intro plays once when the
    // race starts, then the music loops behind it. 0 when the catalogue did not name the
    // circuit, and the caller then leaves whatever is playing alone.
    int intro_track = 0;
    int music_track = 0;

    // ⚠️ WHAT THE PRESENTATION SCREEN SAYS ABOUT THE CIRCUIT, already localised: the `VIE_*`
    // string of its `TOURN.TXT` line - "EASY - A DIRT TRACK WITH PLENTY OF JUMPS AND HILLS."
    // Empty when the catalogue does not name the circuit; a screen then shows nothing rather
    // than a key. Uppercased like `title`, for the same reason (the font has no lowercase).
    std::string description;
    // 0 easy, 1 medium, 2 hard, -1 unknown - formats/tourn.h's difficulty_of(), on the English
    // description. What the random championship draws by.
    int difficulty = -1;

    // Where it sits on the globe, straight from `TOURN.TXT` - see formats/tourn.h for what the
    // two binary angles mean and why `planet` replaces them entirely on MOON1/2/3.
    int globe_x = 0, globe_y = 0;
    int planet = 0;
    // Its hand-written name in `GLOBESCN.SCN`'s NAMES.TM/NAMES2.TM sheet, or -1.
    int name_index = -1;

    // ⚠️ NOT EVERY PLAYABLE CIRCUIT IS ON THE GLOBE. `CITY3` loads perfectly well and is in
    // no `track` line at all, and `TEST1` is the authors' own sandbox: neither has a country, a
    // name sprite or a position, and 0,0 is the NORTH POLE, not "unknown". A screen that showed
    // them there would be stating something the data never says.
    bool on_globe() const { return planet == 0 && name_index >= 0; }

    // How many racers its grid can start in a network race: six, plus the extra places
    // extend_grid() (sim/race.h) finds good. A table rather than a computation because finding
    // them needs the whole level loaded; MEASURED by physics_bench's GRILLE block on
    // 2026-09-25, and to be measured again if extend_grid changes.
    int max_racers = 6;
};

// Reads every shipped circuit and keeps the ones that can actually be played. It unpacks each
// `.LEV` whole and reads the script - about a second for the 26 of them, which is why the answer
// is CACHED for the process, per game folder: the globe, the lobby, the championship and every
// race of a series ask for it, and the files do not change under a running game.
std::vector<LevelEntry> scan_levels(const std::string& game_dir);
// `TOURN.TXT`, out of `TOURN.WAP` - empty when the archive is missing (formats/tourn.h).
TournCatalog load_tourn_catalog(const std::string& game_dir);
int level_max_racers(const std::string& name);

// --- circuits that are not a shipped .LEV (code review, A4) ------------------------------------
// A circuit's files, named as inside a .LEV: at least MAP.MPH, MAP.MPC, MAP.COL and `<name>.TXT`
// (LevelScene::load_files says what the rest is for). The map's size in texels, which MAP.MPH
// must hold exactly; 0 x 0 = square, as every shipped map is.
struct LevelFiles {
    std::unordered_map<std::string, Blob> files;
    int map_w = 0, map_h = 0;
};
// Declares a circuit that is not a shipped .LEV - a generated one. It joins scan_levels()'s list
// for this game folder, at the end, for as long as the process runs; one of the same name is
// replaced. `entry` says what the menus show (name, title, cars, max_racers); `files` is the
// circuit itself.
void add_level(const std::string& game_dir, LevelEntry entry, LevelFiles files);
// A circuit's files: its shipped .LEV unpacked, or an added circuit's own, which add_level
// declared under that same name (formats/game_files.h). Never null, empty when neither can be
// read - never throws.
ArchivePtr level_files(const std::string& game_dir, const std::string& name);
// An added circuit's map size (0 x 0 for a shipped one, which is square).
void level_map_size(const std::string& game_dir, const std::string& name, int& w, int& h);

} // namespace bigred
