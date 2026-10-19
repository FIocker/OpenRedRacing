// The network game's wire format: what a lobby and a lockstep race put in a packet.
//
//
// Everything here is plain data plus a byte encoding - no socket, no ENet. That keeps the
// format testable on its own and lets the bench and the game agree on it by construction.
//
// ⚠️ LITTLE-ENDIAN, FIXED WIDTH, NO PADDING. Every field is written byte by byte by NetWriter,
// never memcpy'd from a struct, so two builds with different packing still read each other.
// ⚠️ BUMP kProtocolVersion on ANY change to a message or to the simulation: a machine running
// a different simulation desynchronises on the first tick, and the version is how the lobby
// refuses it up front instead of letting the race diverge.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/race_rules.h"

namespace bigred {

constexpr uint16_t kProtocolVersion = 9;   // 2: resync; 3: driver in NetLook; 4: INPUTS; 5: airborne damping; 6: own logo; 7: wrecks; 8: the rules in Start; 9: the speed class

// The race's ENet port and the discovery port. A second host on the SAME machine takes the
// next free game port (NetGame::host), so two games can be tested side by side.
// ⚠️ THE DISCOVERY PORT IS BELOW THE GAME PORTS, not above: game ports go up from 47315 for
// each extra host on a machine, and 47316 was once both.
constexpr uint16_t kGamePort = 47315;
// ui/team_logos.h's kCustomLogoMaxBytes, repeated: the wire format does not include the UI.
constexpr size_t kNetCustomLogoMaxBytes = 8192;
constexpr uint16_t kDiscoveryPort = 47314;
constexpr int kGamePortTries = 8;

// Six by default, the original's grid; the host may open up to twelve, and places 6..11 are
// the level's grid continued backwards (sim/race.h, grid_slot).
constexpr int kNetDefaultPlayers = 6;
constexpr int kNetMaxPlayers = 12;

// The input delay, in 28 ms ticks. 3 = 84 ms: far more than a LAN's round trip through the
// host, and still under what a driver notices.
constexpr int kNetInputDelay = 3;

// A SERIES of network races - up to 3 (the user's choice; the original allowed 5). Points by
// finishing place, read out of the original game: its table holds 10,8,6,4,2,0, and it
// adds `table[place]` to each racer's total after every race of mode 0x20/0x23.
// ⚠️ NOT `TOURN.TXT`'s `network` line, which says 10,5,3,2,1,0 and is never read (the
// original's TOURN.TXT reader has no case for `points`). A place past the sixth scores nothing.
constexpr int kNetMaxSeries = 3;
constexpr uint8_t kNetSeriesPoints[6] = {10, 8, 6, 4, 2, 0};
inline int net_series_points(int place) {
    return (place >= 1 && place <= 6) ? kNetSeriesPoints[place - 1] : 0;
}
// One checksum every this many ticks goes to the host, which compares it with its own.
constexpr uint32_t kNetChecksumEvery = 16;

// Channel 0, reliable and ordered, carries everything EXCEPT the race inputs: lobby, START, GO,
// checksums, resync. Channel 1 carries INPUTS, UNRELIABLE: every packet repeats all the inputs
// the other end has not acknowledged yet, so a lost packet is covered by the next one, 28 ms
// later, instead of stalling the race for a resend and everything queued behind it (the
// latency model: 1 % of loss was enough to
// show "WAITING FOR ..." at 0 ms of ping). ⚠️ Being on another channel, an INPUTS packet can
// overtake the START of its race or the RESYNC announcement: it carries the race id and the
// last resync tick for exactly that reason.
constexpr uint8_t kChannel = 0;
constexpr uint8_t kInputChannel = 1;
constexpr uint8_t kChannelCount = 2;

enum class NetMsg : uint8_t {
    // client -> host
    Hello = 1,        // u16 version, player look
    SetLook = 2,      // player look
    SetReady = 3,     // u8
    SetVehicle = 4,   // u8 index into the level's own `car` list
    Loaded = 5,       // u32 race id: this machine has built the race and waits for GO
    // host -> client
    Welcome = 20,     // u8 your id
    Refuse = 21,      // u8 NetRefusal
    Lobby = 22,       // the whole lobby, sent on every change
    Start = 23,       // NetRaceStart
    Go = 24,          // u32 race id: every machine is loaded, tick 1 starts now
    Desync = 25,      // u32 tick: a checksum disagreed
    ResyncAt = 26,    // u32 tick R: every machine stops after tick R for the host's snapshot
    Snapshot = 27,    // u32 tick R, blob: the host's whole race after tick R (sim/snapshot.h)

    // both ways, in a race
    Checksum = 41,    // u8 racer, u32 tick, u32 hash
    // UNRELIABLE, channel 1: u32 race id, u32 last resync tick (host -> client; 0 from a client),
    // u8 n, n x (u8 racer, u32 tick): "I hold every input of this racer up to this tick",
    // u8 m, m x (u8 racer, u32 first tick, u8 count, count x u8 bits): inputs, in tick order.
    Inputs = 42,
};

enum class NetRefusal : uint8_t { None = 0, Version = 1, Full = 2, Racing = 3 };

// The lobby's phases. The host moves it forward; a client only ever follows.
enum class NetPhase : uint8_t {
    Lobby = 0,             // players come and go, set themselves ready
    ChoosingVehicles = 1,  // everyone was ready: each picks a vehicle
    Racing = 2,            // START sent: loading, then the race
};

// What a player looks like and chooses - their profile, announced. Nothing here reaches the
// simulation except `damage`, which is why it is carried in the START message too.
struct NetLook {
    std::string name;
    uint8_t colour = 0;   // index into color_presets()
    uint8_t logo = 0;     // index into the game's 13 team logos
    uint8_t damage = 30;  // 0 (SOLIDE) .. 100 (FRAGILE), the player's own option
    uint8_t character = 0;         // 0..5, the driver whose head rides in the vehicle
    uint8_t character_colour = 0;  // index into color_presets()
    // The player's own logo, as PNG bytes, when `logo` is kTeamLogoCount (ui/team_logos.h):
    // the others have no copy of the file. Capped at 8 KB on both ends.
    std::vector<uint8_t> custom_logo;
};

struct NetPlayer {
    uint8_t id = 0;           // stable for the whole session; the host is 0
    NetLook look;
    bool ready = false;
    int16_t vehicle = -1;     // index into the level's `car` list, -1 = not chosen yet
    uint16_t points = 0;      // this series so far - see kNetSeriesPoints
};

// What only the host sets.
struct NetSettings {
    std::string level = "DIRT1";
    uint8_t laps = 0;         // 0 = the level's own count
    bool ghosts = false;      // fill the empty grid places with the level's recorded ghosts
    uint8_t max_players = kNetDefaultPlayers;
    uint8_t races = 1;            // races in the series, 1..kNetMaxSeries
    bool random_circuit = false;  // the host's machine draws each race's circuit
};

struct NetLobbyState {
    NetPhase phase = NetPhase::Lobby;
    NetSettings settings;
    // Races already run in this series. Equal to settings.races once it is over: the lobby then
    // shows the final standings until the host starts the next series.
    uint8_t series_done = 0;
    bool series_over() const { return series_done >= settings.races && series_done > 0; }
    std::vector<NetPlayer> players;   // the host first
};

// One human racer of a race about to start.
struct NetRacer {
    uint8_t player_id = 0;
    uint8_t grid = 0;         // the grid block, 0..11
    std::string vehicle;      // the vehicle's name
    NetLook look;
};

struct NetRaceStart {
    uint32_t race_id = 0;
    NetSettings settings;
    uint8_t delay = kNetInputDelay;
    std::vector<NetRacer> racers;   // INDEX = the racer's lockstep index
    // The rules every machine plays this race by - the host's (NetGame::set_rules).
    RaceRules rules = RaceRules::network();
};

// --- encoding ---------------------------------------------------------------------------------

class NetWriter {
public:
    void u8(uint8_t v) { b_.push_back(v); }
    void u16(uint16_t v) { u8(static_cast<uint8_t>(v)); u8(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { u16(static_cast<uint16_t>(v)); u16(static_cast<uint16_t>(v >> 16)); }
    void str(const std::string& s);   // u8 length, then the bytes; longer is cut at 255
    void blob(const std::vector<uint8_t>& b);   // u32 length, then the bytes
    const std::vector<uint8_t>& bytes() const { return b_; }

private:
    std::vector<uint8_t> b_;
};

// Reads until it runs out; every read past the end returns 0 and sets `ok()` false, so a short
// or hostile packet is dropped by one check at the end instead of one per field.
class NetReader {
public:
    NetReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    std::string str();
    std::vector<uint8_t> blob();
    bool ok() const { return ok_; }
    bool at_end() const { return p_ == end_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

void write_look(NetWriter& w, const NetLook& l);
NetLook read_look(NetReader& r);
void write_lobby(NetWriter& w, const NetLobbyState& s);
bool read_lobby(NetReader& r, NetLobbyState& s);
void write_start(NetWriter& w, const NetRaceStart& s);
bool read_start(NetReader& r, NetRaceStart& s);

} // namespace bigred
