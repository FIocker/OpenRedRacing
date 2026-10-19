// The six drivers: which body and which head each one is, and the shapes out of CHARSCN.SCN.
//
//
// ⚠️ CONFIRMED, NOT GUESSED: the original switches on the character number and picks
// one of four body tables (BLOKE, BLOKEB, WOMAN, WOMANB - six colour variants each) and one head - HEAD1<colour> and HEAD4<colour> come in the six colours too, the
// other four are single shapes. The same switch sets a height for the body and one for the head;
// their difference is where the head sits on the shoulders.
//
// The same head is what a vehicle carries at its `&40` anchor (scene/vehicle_render.h's
// VehicleDriver): the driver in the menu and the one at the wheel are one shape.
#pragma once

#include <string>
#include <unordered_map>

#include "formats/bigred_archive.h"
#include "formats/threedw.h"

namespace bigred {

constexpr int kCharacterCount = 6;

struct CharacterInfo {
    const char* first_name;   // what a list shows when the text table is missing
    const char* bio_key;      // `CHAR1`..`CHAR6` of TOURN.<LANG>: the whole fact sheet
    const char* body;         // table stem: BLOKE, BLOKEB, WOMAN, WOMANB (+ colour digit)
    const char* head;         // shape stem; + colour digit when `head_coloured`
    bool head_coloured;
    // The original's two heights, in its own units: the body's and the head's. Only their
    // DIFFERENCE is used - how far above the body's origin the head's origin sits.
    int body_height;
    int head_height;
};

// Wraps: any integer names one of the six.
const CharacterInfo& character_info(int index);

// One driver, ready to be turned into meshes: the body (colour variant 1 - a player's colour is
// a regenerated ramp, never a baked variant) and the head, plus the sheet the mouths are cut
// from.
struct CharacterShapes {
    ThreeDW body;
    ThreeDW head;
    Blob mouth_tm;
    // How far the head's origin is above the body's, in LOCAL RENDER units (+Y up, .3DW
    // halved) - the space scene/vehicle_render.h's meshes are in.
    float head_rise = 0.0f;
    bool valid = false;
};

// Reads the driver out of `<game_dir>\CHARSCN.SCN` (unpacked once, formats/game_files.h).
bool load_character_shapes(const std::string& game_dir, int index, CharacterShapes& out);

} // namespace bigred
