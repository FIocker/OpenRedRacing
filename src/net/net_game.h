// A network game: the lobby, then lockstep races, over ENet. One object per machine, host or
// client, living for as long as the player stays in the game - across the lobby, the vehicle
// choice, the race and its results.
//
//
// ⚠️ A STAR, NOT A MESH. Every client talks to the host only; the host relays each racer's
// inputs to the others. Inputs travel UNRELIABLE and repeated until acknowledged (INPUTS in
// net_protocol.h); the host relays only what it holds WITHOUT A GAP, so every machine sees
// the same inputs for a racer, the host's. On a LAN the extra hop costs a millisecond, and it buys three things:
// one connection per client, one place that knows who is still there, and one place that fills
// in a departed racer's inputs so the others never wait for someone who has gone.
//
// ⚠️ NOTHING HERE READS A CLOCK FOR THE SIMULATION. `service()` takes the display's millisecond
// clock for the discovery and nothing else; the race advances only on the ticks the screen
// runs, and only when LockstepInputs says every input for that tick is in.
//
// ⚠️ SERVICE IT EVERY FRAME, WHATEVER SCREEN IS UP. The App does (app/app.h): the connection
// has to stay alive under the results screen and the vehicle screen, which know nothing of it.
#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <string>
#include <vector>

#include "discovery.h"
#include "net_protocol.h"
#include "sim/lockstep.h"

struct _ENetHost;
struct _ENetPeer;

namespace bigred {

class NetGame {
public:
    NetGame();
    ~NetGame();
    NetGame(const NetGame&) = delete;
    NetGame& operator=(const NetGame&) = delete;

    // --- opening and closing ------------------------------------------------------------
    // `loopback_only` keeps everything on 127.0.0.1: the bench's tests never open a port to the
    // network. A second host on one machine takes the next free port.
    bool host(const NetLook& me, const NetSettings& settings, bool loopback_only = false);
    bool join(const std::string& address, uint16_t port, const NetLook& me);
    // Leaves politely (the others hear a disconnect at once, not after a timeout).
    void leave();

    void service(uint32_t now_ms);
    // Sends whatever is queued now, rather than at the next service(). The race screen calls it
    // right after its inputs: service() runs BEFORE the screen's update, so without this every
    // input would sit a whole frame in the queue before leaving.
    void flush();

    bool active() const { return host_ != nullptr; }
    bool is_host() const { return role_ == Role::Host; }
    // A client is IN once the host has welcomed it. The host always is.
    bool joined() const { return is_host() || welcomed_; }
    // Set once the game is over for this machine: refused, host gone, connection lost.
    const std::string& error() const { return error_; }
    uint16_t port() const { return port_; }
    uint8_t local_id() const { return local_id_; }

    // --- the lobby ------------------------------------------------------------------------
    const NetLobbyState& lobby() const { return lobby_; }
    // Bumped on every change a screen should redraw for.
    uint32_t revision() const { return revision_; }
    const NetPlayer* player(uint8_t id) const;
    const NetPlayer* me() const { return player(local_id_); }

    void set_look(const NetLook& look);
    void set_ready(bool ready);
    void set_vehicle(int16_t index);

    // Host only. `cars` is the level's own list (level_catalog.h) - the host turns everyone's
    // index into a vehicle name with it when the race starts.
    void set_settings(const NetSettings& s, std::vector<std::string> cars);
    // HOST: the rules the next races are played by, sent in their start message. The network
    // defaults unless the host asks otherwise (sim/race_rules.h).
    void set_rules(const RaceRules& r) { rules_ = r; }
    const RaceRules& rules() const { return rules_; }
    bool everyone_ready() const;
    // Everyone ready -> everyone picks a vehicle. The race starts on its own once all have.
    bool begin_vehicle_choice();
    // Back to the lobby from any phase: readiness and vehicles are cleared.
    void back_to_lobby();
    // Host: a race of the series is over - each human's finishing place, by player id. Adds
    // the points (kNetSeriesPoints) and counts the race. Called once per race, from the race
    // screen, before the lobby comes back.
    void record_race(const std::vector<std::pair<uint8_t, int>>& places);

    // --- a race ------------------------------------------------------------------------
    // Set from START until the race screen takes it.
    const NetRaceStart* pending_start() const { return start_pending_ ? &start_ : nullptr; }
    void take_start() { start_pending_ = false; }
    const NetRaceStart& race() const { return start_; }
    int local_racer() const { return local_racer_; }
    // The race screen has built everything: tell the host. The race starts when all have.
    void race_loaded();
    bool race_go() const { return go_; }
    LockstepInputs& inputs() { return inputs_; }
    // The LOCAL racer's input for `tick` (already scheduled - LockstepInputs::schedule).
    void send_input(uint32_t tick, uint8_t bits);
    void report_checksum(uint32_t tick, uint32_t hash);
    // The first tick two machines disagreed on, or 0. Back to 0 once the resync is through.
    uint32_t desync_tick() const { return desync_tick_; }

    // --- the resync ---------------------------------------------------------------------------
    // After a desync the host names a tick R no machine can have run yet (just past the last
    // input it has sent - the channel is ordered, so the announcement arrives before anything
    // that would let a client go further). Every machine runs up to R; the host then sends its
    // race as it stands after R, and the others replace theirs with it before running R + 1.
    // 0 when none is under way.
    uint32_t resync_tick() const { return resync_tick_; }
    // Host: the race after tick R, frozen - sent to everyone, and the resync is over.
    void send_snapshot(uint32_t tick, const std::vector<uint8_t>& race);
    // Client: the host's snapshot for tick R once it has arrived, else null.
    const std::vector<uint8_t>* snapshot_for(uint32_t tick) const {
        return (snapshot_tick_ == tick && tick != 0) ? &snapshot_ : nullptr;
    }
    // Client: the snapshot is loaded (or could not be) - the race runs on.
    void snapshot_applied(bool ok);
    uint32_t resyncs() const { return resyncs_; }
    // False once the racer's machine has left; the host drives it with nothing pressed.
    bool racer_present(int racer) const;

    // TESTING: throw away this percentage of the INPUTS packets this machine sends, as a lossy
    // network would (net_bench, and the game's -NETLOSS=N). A fixed sequence, not a clock.
    void set_input_loss(int percent) { input_loss_ = percent; }

    // Counters for the bench and the HUD.
    struct Stats {
        uint32_t inputs_sent = 0, inputs_received = 0, checksums_compared = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    enum class Role { None, Host, Client };
    // One end of the INPUTS exchange: what the other end has told us it holds, per racer, and
    // when we last wrote to it. A client has one (to the host), the host one per client.
    struct InputLink {
        std::vector<uint32_t> acked;      // per racer: the other end holds every input up to here
        std::vector<uint32_t> sent_upto;  // per racer: the furthest input already sent once
        std::vector<uint32_t> told;       // per racer: the holding we last reported to it
        uint32_t last_ms = 0;
        void reset(size_t racers, uint32_t delay) {
            acked.assign(racers, delay);
            sent_upto.assign(racers, delay);
            told.assign(racers, delay);
            last_ms = 0;
        }
    };
    struct Peer {
        _ENetPeer* peer = nullptr;
        uint8_t id = 0;
        bool welcomed = false;
        InputLink link;
    };

    void reset_state();
    void close_host();
    void send(_ENetPeer* to, const NetWriter& w);
    void send_all(const NetWriter& w, _ENetPeer* except = nullptr);
    void on_receive(_ENetPeer* from, const uint8_t* data, size_t size);
    void host_receive(Peer& from, NetMsg type, NetReader& r);
    void client_receive(NetMsg type, NetReader& r);
    void on_disconnect(_ENetPeer* peer);
    Peer* find_peer(_ENetPeer* p);
    void lobby_changed();          // host: bump and broadcast
    void maybe_start_race();       // host: every vehicle chosen -> START
    void maybe_go();               // host: every racer loaded -> GO
    void fill_departed();          // host: nothing-pressed inputs for racers who left
    // Every input of the race, as this machine holds it: into LockstepInputs AND the history
    // the INPUTS packets are cut from. Returns false if it was already held.
    bool hold_input(int racer, uint32_t tick, uint8_t bits);
    void send_inputs(uint32_t now_ms);                        // every link that needs it
    void send_inputs_to(_ENetPeer* to, InputLink& link, int skip_racer, uint32_t now_ms);
    void receive_inputs(InputLink& link, int only_racer, NetReader& r);
    bool race_configured() const { return !contig_.empty(); }
    void compare_checksum(uint32_t tick, uint32_t hash);
    int racer_of(uint8_t player_id) const;
    void configure_race();

    Role role_ = Role::None;
    _ENetHost* host_ = nullptr;
    _ENetPeer* server_ = nullptr;   // a client's connection to the host
    std::vector<Peer> peers_;       // the host's connections
    DiscoveryResponder responder_;
    uint16_t port_ = 0;
    std::string error_;

    NetLook my_look_;
    uint8_t local_id_ = 0;
    uint8_t next_id_ = 1;
    bool welcomed_ = false;
    NetLobbyState lobby_;
    std::vector<std::string> cars_;
    uint32_t revision_ = 0;

    NetRaceStart start_;
    RaceRules rules_ = RaceRules::network();
    bool start_pending_ = false;
    uint32_t next_race_id_ = 0;
    int local_racer_ = -1;
    LockstepInputs inputs_;
    std::vector<uint8_t> loaded_;      // host: per racer
    std::vector<uint8_t> present_;     // per racer
    // Per racer, a window of the inputs held (0x100 | bits, 0 = not held), and the tick up to
    // which they are held WITHOUT A GAP - the only ones the host relays and anyone acknowledges.
    static constexpr uint32_t kHistory = LockstepInputs::kWindow;
    std::vector<std::vector<uint16_t>> history_;
    std::vector<std::vector<uint32_t>> history_tick_;
    std::vector<uint32_t> contig_;
    InputLink server_link_;            // client: the exchange with the host
    uint32_t last_local_tick_ = 0;
    uint32_t last_resync_ = 0;         // host: the last R announced; client: the last R applied
    uint32_t now_ms_ = 0;              // the last service()'s clock, for flush()
    bool inputs_dirty_ = false;        // a local input not sent yet
    int input_loss_ = 0;               // TESTING: percent of INPUTS packets dropped
    uint32_t loss_seed_ = 12345;
    bool go_ = false;
    std::map<uint32_t, uint32_t> checks_;   // host: tick -> the first hash reported for it
    uint32_t desync_tick_ = 0;
    uint32_t resync_tick_ = 0;
    uint32_t check_floor_ = 0;          // host: checksums of ticks up to here are not compared
    std::vector<uint8_t> snapshot_;     // client: the host's race...
    uint32_t snapshot_tick_ = 0;        // ...after this tick
    uint32_t resyncs_ = 0;
    Stats stats_;
};

} // namespace bigred
