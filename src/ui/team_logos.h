// `LOGOS.SPR` - the game's own team logos, and the hand-drawn plates they sit on.
//
//
// One `.SPR` of 63 glyphs inside `LOGOSCN.SCN`, and its three parts are told apart by SIZE
// alone - the file carries no index, no names and no groups:
//
//   0..12   40x40  THE LOGOS: a skull, a rabbit, a diamond pattern, a star, a shield, a rising
//                  sun, a dragon, a flower, a smiley, a yin-yang, "RUF TUFF", a sun-and-yin-yang
//                  and a blue mask. Thirteen, and that is the whole set the original offers.
//   13..42  56x56  SIX COLOURED PLATES, five animation frames each - the wobbly hand-drawn
//                  square the logo is shown on. Six is not a coincidence: they are the six team
//                  colours of the repaints (scene/vehicle_render.h).
//   43..62  24x24 and up  the screen's own arrows and OK stamps, five frames each. Not ours:
//                  `LOGOSCN.SCN` is not a screen this engine rebuilds.
//
// THE PLAYER'S OWN LOGO comes after the thirteen, as a fourteenth: a PNG of exactly 40x40 at
// one fixed place (App gives it: `%APPDATA%\OpenBigRed\logo.png`), taken if it is there and
// valid, silently absent otherwise - the user's own rule, "le plus simple". Every screen that
// walks count() offers it without knowing. In a network game it travels as PNG bytes in the
// player's NetLook, so the others paint it too (see custom_logo_png / decode_logo_png).
//
// ⚠️ THE PALETTE IS `TEAM.RAW`'s, the background of the screen the sheet belongs to. A `.SPR`
// carries none - see formats/sprite.h. The logos are shown on other screens here, so they are
// decoded through their own screen's palette and uploaded as RGBA, rather than being palette
// indices looking for a table.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "formats/sprite.h"
#include "formats/threedw.h"   // RGB

namespace bigred {

// What the sheet holds, by the sizes above. Not guessed: counted. It is also the INDEX the
// player's own logo takes - the one after the game's - in a profile and on the wire.
constexpr int kTeamLogoCount = 13;
// The player's own logo: its side, and the most PNG bytes it may take on the wire.
constexpr int kCustomLogoSide = 40;
constexpr size_t kCustomLogoMaxBytes = 8192;

// Where the player's own logo is read from. Set once by App, before any screen loads logos.
void set_custom_logo_path(const std::string& path_utf8);
// The player's own logo as PNG bytes - re-encoded from the decoded image, so what goes on the
// wire is always small and always valid. Empty when there is none. Read once, then cached.
const std::vector<uint8_t>& custom_logo_png();
// Any PNG to 40x40 RGBA8. False - and nothing drawn - for anything else: wrong size, too many
// bytes, not a PNG. Used for our own file and for what another player sent.
bool decode_logo_png(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba);
constexpr int kTeamPlateColours = 6;
constexpr int kTeamPlateFrames = 5;

// The names a logo and a plate are registered under with the interface renderer. Shared, so
// that two screens showing the same emblem ask for the same image instead of each uploading its
// own copy under its own name.
inline std::string team_logo_image_name(int index) {
    return "logo:" + std::to_string(index);
}
inline std::string team_plate_image_name(int colour, int frame) {
    return "logo:plate" + std::to_string(colour) + "_" + std::to_string(frame);
}

class TeamLogos {
public:
    // Unpacks `LOGOSCN.SCN` and keeps the sheet and its palette. False means the artwork is not
    // there; a screen must still work without it, so nothing here is fatal.
    bool load(const std::string& game_dir);
    bool ready() const { return !glyphs_.empty() && !palette_.empty(); }

    // How many logos can be chosen: 13 on the shipped game, 14 with the player's own, 0 on a
    // copy without the file.
    int count() const;
    // The game's own, without the player's. The player's own, when there is one, is this index.
    int builtin_count() const;

    // Logo `index`, RGBA8, with index 0 transparent. `out_w`/`out_h` receive its size.
    std::vector<uint8_t> logo_rgba(int index, int& out_w, int& out_h) const;
    // The plate under it: `colour` 0..5, `frame` 0..4.
    std::vector<uint8_t> plate_rgba(int colour, int frame, int& out_w, int& out_h) const;

    // Which of the six plates is closest to an arbitrary RGB - the player's own team colour is
    // one of twenty-two presets now, and the plates are six.
    //
    // ⚠️ MEASURED FROM THE ARTWORK, not typed in: the plate's colour is the commonest opaque
    // index in its own pixels, read through the sheet's palette. A table written by hand here
    // would be a second description of a file we already have.
    int nearest_plate(uint8_t r, uint8_t g, uint8_t b) const;

private:
    // Where each family starts in the sheet, resolved by size at load.
    int logo_first_ = -1, plate_first_ = -1;
    std::vector<uint8_t> custom_rgba_;   // the player's own logo, 40x40, or empty
    std::vector<SpriteGlyph> glyphs_;
    std::vector<RGB> palette_;
    RGB plate_colour_[kTeamPlateColours]{};
};

} // namespace bigred
