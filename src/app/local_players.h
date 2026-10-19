// The people playing at THIS machine - one, or up to four on a split screen - and which
// controls each of them drives with.
//
//
// ⚠️ A SESSION THING, NOT A PROFILE THING. It is set on the split-screen screen, carried
// through the globe and the presentation screen into the race, and dropped by the main menu on
// the way back. Only the first player has a profile on disk; the others are guests with a
// colour, a driver and a name.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bigred {

// What one player drives with. The keyboard is shared, so it is split in two halves; each pad
// belongs to one player. `Everything` is the one-player rule: keyboard and every pad at once.
//
// ⚠️ KEYS BY POSITION, NOT BY LETTER: SDL scancodes name where a key is, so the left half is
// W A S D on a QWERTY board and Z Q S D on the user's AZERTY one - the same four keys under the
// same fingers.
enum class Controls : uint8_t {
    Everything,
    KeyboardRight,   // the arrows; right Ctrl = nitro, right Shift = back on the track, End = view
    KeyboardLeft,    // W A S D (Z Q S D); left Shift = nitro, left Ctrl = back, C = view
    Pad1,
    Pad2,
    Pad3,
    Pad4,
};
constexpr int kControlsCount = 7;

// What a screen writes: "ARROWS", "PAD 2"...
const char* controls_name(Controls c);
// Pad1..Pad4 -> 0..3, anything else -> -1.
int controls_pad(Controls c);

constexpr int kMaxLocalPlayers = 4;

struct LocalSeat {
    std::string name;            // "PLAYER 2"; the first seat takes the profile's pseudo
    Controls controls = Controls::Everything;
    int character = 0;           // scene/character_model.h
    int character_color = 0;     // index into color_presets()
    int vehicle_color = 0;
    int logo_index = 0;
    // The vehicle chosen on the split vehicle screen, as an index into the circuit's list;
    // -1 before anyone chose (game/split_screen.h).
    int vehicle = -1;
};

struct LocalPlayers {
    std::vector<LocalSeat> seats;
    // The series they play is not here but in the session's competition
    // (game/competition.h, Competition::series).
    bool split() const { return seats.size() > 1; }
    void clear() { seats.clear(); }
};

} // namespace bigred
