// The player's profile: pseudo, avatar/vehicle colours, team logo - everything the original
// asked for once, piecemeal, across NAMESCN/CHARSCN/LOGOSCN, and that OpenBigRed instead asks
// for once and keeps.
//
//
// ⚠️ NOT a game asset. It lives OUTSIDE the (read-only) game directory, under the player's own
// profile - see profile_dir(). It is plain text, `key value` per line, `;` comments - the same
// convention `BIGRED.CFG` itself uses (formats/localised_text.h), on purpose: it is meant to be
// opened in Notepad and edited by hand.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "app/presentation.h"   // kDefaultDisplayMode
#include "sim/race_rules.h"       // kSpeedClasses

namespace bigred {

// A curated set of team-colour presets, sober and loud mixed together. Each is a name (shown in
// the picker) and a flat RGB.
//
// ⚠️ THAT RGB IS THE BRIGHTEST STEP OF A RAMP, not a flat paint. Since 2026-09-23 the vehicle
// renderer generates the authentic 7-step luminance ramp from it and walks each bodywork face
// onto its own step, exactly as the six shipped repaints do - see
// `scene/vehicle_render.h`'s VehicleTint. So a car really is
// painted in the chosen colour, with the artists' fake per-facet shading intact, and the six
// original liveries stop being the only possibilities.
//
// ⚠️ THE CHARACTER's colour is still only a swatch: the driver's head is `&40`, taken from the
// global shape table, and nothing draws it yet.
struct ColorPreset {
    const char* name;
    uint8_t r, g, b;
};
const std::vector<ColorPreset>& color_presets();

// The first player's driving keys (Options -> Controls), in KEYSCN's own order: accelerate,
// brake, left, right, nitro, back on the track. SDL SCANCODES - positions on the board, like
// every key in this engine (app/local_players.h) - and a Ctrl, Alt, Shift or GUI key stands for
// both of its sides. Only the keyboard: the pad's buttons are positions already, the same on
// every pad, and the split screen's two halves keep their fixed keys.
enum DriveKey { kKeyAccel, kKeyBrake, kKeyLeft, kKeyRight, kKeyNitro, kKeyRecover, kDriveKeyCount };
// The arrows, Ctrl and Alt: the user's choice of 2026-09-15.
std::array<int, kDriveKeyCount> default_drive_keys();
// The profile's name for each, "key_accel".."key_recover".
const char* drive_key_setting(int action);

struct PlayerProfile {
    std::string pseudo;               // never empty once saved - the create screen enforces it
    int character = 0;                // 0..5, which of the six drivers (scene/character_model.h)
    int character_color = 0;          // index into color_presets(), independent of the vehicle
    int vehicle_color = 0;            // "si le vehicule est rouge, le personnage est rouge
                                       // aussi" was a shared convention in the original data,
                                       // never a code link - the two are free here (see spec)
    // ⚠️ TWO SEPARATE THINGS, and they will stay separate. `logo_index` picks one of the
    // THIRTEEN logos the game ships (ui/team_logos.h) and is what the vehicle screen offers
    // today; `logo` names a PNG the player dropped in logo_dir(), which is the planned design
    // and is not built yet. A custom logo, when it exists, wins
    // over the index - so the index is never lost by trying one.
    int logo_index = 0;               // 0..12 the game's own LOGOS.SPR, 13 the player's logo.png
    std::string logo;                 // reserved, never read (ui/team_logos.h has logo.png)
    bool hide_other_logos = false;    // client-side only, see spec

    // ⚠️ SETTINGS, NOT JUST IDENTITY: the user asked for "les reglages" alongside the pseudo and
    // the colours, so anything the engine already exposes a knob for is saved here too rather
    // than starting a second config file. Defaults mirror each system's OWN built-in default
    // (app/presentation.h's kDefaultDisplayMode, audio/music.h's 192, audio/mixer.h's 256), so
    // loading a freshly-created profile changes nothing.
    int display_mode = kDefaultDisplayMode;   // the +/- ladder rung, app/presentation.h
    int music_volume = 192;                   // 0..256, audio/music.h
    int fx_volume = 256;                      // 0..256, audio/mixer.h's set_master()

    // ⚠️ DAMAGE LIVES HERE, NOT IN A SEPARATE "RACE OPTIONS" SCREEN - decided by the user
    // 2026-09-23 after seeing the original's `GAMESCN`: solo, it is simply the player's own
    // setting; in shared-screen multiplayer each player sets their OWN from their own corner,
    // which is exactly a per-profile field passed
    // into the race, not a global switch. `30` mirrors sim/vehicle.h's
    // `VehicleSim::kDefaultDamageOption` (the original's own value, not guessed) - the meter
    // GAMESCN itself shows at that same default value.
    int damage_option = 30;                   // 0 (SOLIDE) .. 100 (FRAGILE)
    // The original's SOUNDSCN COMMENTAIRES checkbox: the circuit's spoken lines, READYGO and
    // the silly word said on every menu choice.
    bool commentary = true;
    // Two players on a split screen: one above the other (the default) or side by side. Chosen
    // in player one's quarter of the split screen, remembered here (user, 2026-09-30).
    bool split_side_by_side = false;
    // The speed class, "Mario Kart CC" (user, 2026-09-30): an index into sim/race_rules.h's
    // kSpeedClasses, 1 = 100CC = the original. Every race this player starts is played at it -
    // alone, the championship, a split screen, and a network game they host.
    int speed_class = 1;
    std::array<int, kDriveKeyCount> keys = default_drive_keys();

    bool valid() const { return !pseudo.empty(); }
};

// Where a player's files live: `%APPDATA%\OpenBigRed\` (SDL_GetPrefPath). Not the game
// directory - that one is read-only original media (CLAUDE.md's hard constraint) and is not
// even writable on a shared/original install.
std::string profile_dir();
std::string profile_path();   // profile_dir() + "profile.cfg"
std::string logo_dir();       // profile_dir() + "logos\", where a dropped PNG becomes selectable

bool profile_exists();

// Defaults every field a missing key does not set - a hand-edited file missing "vehicle_color"
// gets 0, not a parse failure.
PlayerProfile load_profile();

// Creates profile_dir() (and logo_dir()) if needed. False means the write failed - caller keeps
// what the player already had rather than losing the session's edits.
bool save_profile(const PlayerProfile& profile);

// The RaceRules::speed_percent of the profile's speed class.
inline int32_t speed_percent_of(const PlayerProfile& p) {
    return kSpeedClasses[p.speed_class >= 0 && p.speed_class < kSpeedClassCount ? p.speed_class : 1]
        .percent;
}

} // namespace bigred
