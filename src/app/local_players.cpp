#include "local_players.h"

namespace bigred {

const char* controls_name(Controls c) {
    switch (c) {
    case Controls::Everything: return "ALL";
    case Controls::KeyboardRight: return "ARROWS";
    case Controls::KeyboardLeft: return "W A S D";
    case Controls::Pad1: return "PAD 1";
    case Controls::Pad2: return "PAD 2";
    case Controls::Pad3: return "PAD 3";
    case Controls::Pad4: return "PAD 4";
    }
    return "?";
}

int controls_pad(Controls c) {
    switch (c) {
    case Controls::Pad1: return 0;
    case Controls::Pad2: return 1;
    case Controls::Pad3: return 2;
    case Controls::Pad4: return 3;
    default: return -1;
    }
}

} // namespace bigred
