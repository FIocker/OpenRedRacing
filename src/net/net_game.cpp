#include "net_game.h"

#include <enet/enet.h>

#include <algorithm>
#include <cstdio>

namespace bigred {
namespace {

// Peers that go quiet are declared gone after 10 s at the least - a level load blocks the
// thread for a few seconds and must not cost a player their seat.
constexpr enet_uint32 kTimeoutMin = 10000, kTimeoutMax = 30000;

// INPUTS: with nothing new to say, unacknowledged inputs go again this often.
constexpr uint32_t kInputResendMs = 30;
// And at most this many inputs of one racer in one packet (12 racers x 64 stays under a
// datagram: ENet would otherwise cut a big unreliable packet into RELIABLE fragments).
constexpr uint32_t kInputsPerRun = 64;

// ENet slows unreliable packets down, by DROPPING them, when it thinks a link is congested (its
// packet throttle). Inputs are a few hundred bytes a second: never throttle them.
void never_throttle(ENetPeer* p) {
    enet_peer_throttle_configure(p, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                 ENET_PEER_PACKET_THROTTLE_SCALE, 0);
}

const char* refusal_text(NetRefusal r) {
    switch (r) {
    case NetRefusal::Version: return "THE HOST RUNS ANOTHER VERSION";
    case NetRefusal::Full: return "THE GAME IS FULL";
    case NetRefusal::Racing: return "THE GAME HAS ALREADY STARTED";
    default: return "REFUSED BY THE HOST";
    }
}

} // namespace

NetGame::NetGame() { net_acquire(); }

NetGame::~NetGame() {
    leave();
    net_release();
}

void NetGame::reset_state() {
    role_ = Role::None;
    server_ = nullptr;
    peers_.clear();
    port_ = 0;
    local_id_ = 0;
    next_id_ = 1;
    welcomed_ = false;
    lobby_ = NetLobbyState();
    start_ = NetRaceStart();
    start_pending_ = false;
    local_racer_ = -1;
    go_ = false;
    checks_.clear();
    desync_tick_ = 0;
    resync_tick_ = check_floor_ = snapshot_tick_ = resyncs_ = 0;
    last_resync_ = 0;
    snapshot_.clear();
    history_.clear();
    history_tick_.clear();
    contig_.clear();
    stats_ = Stats();
    ++revision_;
}

void NetGame::close_host() {
    responder_.close();
    if (host_) enet_host_destroy(host_);
    host_ = nullptr;
    server_ = nullptr;
    peers_.clear();
}

bool NetGame::host(const NetLook& me, const NetSettings& settings, bool loopback_only) {
    leave();
    reset_state();
    error_.clear();
    ENetAddress a{};
    if (loopback_only) enet_address_set_host_ip(&a, "127.0.0.1");
    else a.host = ENET_HOST_ANY;
    for (int i = 0; i < kGamePortTries && !host_; ++i) {
        a.port = static_cast<enet_uint16>(kGamePort + i);
        // Twice the seats: a client turned away still needs a connection to be told why.
        host_ = enet_host_create(&a, 2 * kNetMaxPlayers, kChannelCount, 0, 0);
    }
    if (!host_) {
        error_ = "NO FREE NETWORK PORT";
        return false;
    }
    port_ = a.port;
    role_ = Role::Host;
    my_look_ = me;
    local_id_ = 0;
    lobby_.settings = settings;
    if (lobby_.settings.max_players > kNetMaxPlayers) lobby_.settings.max_players = kNetMaxPlayers;
    lobby_.players.push_back(NetPlayer{0, me, false, -1});
    if (!responder_.open(loopback_only))
        std::printf("[net] discovery port %u busy: this game will not be listed, join by address\n",
                    kDiscoveryPort);
    std::printf("[net] hosting on port %u%s\n", port_, loopback_only ? " (loopback only)" : "");
    std::fflush(stdout);
    ++revision_;
    return true;
}

bool NetGame::join(const std::string& address, uint16_t port, const NetLook& me) {
    leave();
    reset_state();
    error_.clear();
    host_ = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (!host_) {
        error_ = "NETWORK UNAVAILABLE";
        return false;
    }
    ENetAddress a{};
    if (enet_address_set_host(&a, address.c_str()) != 0) {
        error_ = "UNKNOWN ADDRESS";
        close_host();
        return false;
    }
    a.port = port;
    server_ = enet_host_connect(host_, &a, kChannelCount, 0);
    if (!server_) {
        error_ = "COULD NOT CONNECT";
        close_host();
        return false;
    }
    enet_peer_timeout(server_, 0, kTimeoutMin, kTimeoutMax);
    role_ = Role::Client;
    my_look_ = me;
    port_ = port;
    std::printf("[net] joining %s:%u\n", address.c_str(), port);
    std::fflush(stdout);
    return true;
}

void NetGame::leave() {
    if (!host_) {
        role_ = Role::None;
        return;
    }
    if (role_ == Role::Host) {
        for (Peer& p : peers_) enet_peer_disconnect(p.peer, 0);
    } else if (server_) {
        enet_peer_disconnect(server_, 0);
    }
    enet_host_flush(host_);
    close_host();
    role_ = Role::None;
    ++revision_;
}

// --- plumbing ---------------------------------------------------------------------------------

void NetGame::send(ENetPeer* to, const NetWriter& w) {
    if (!to) return;
    ENetPacket* p = enet_packet_create(w.bytes().data(), w.bytes().size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(to, kChannel, p);
}

void NetGame::send_all(const NetWriter& w, ENetPeer* except) {
    for (Peer& p : peers_)
        if (p.welcomed && p.peer != except) send(p.peer, w);
}

NetGame::Peer* NetGame::find_peer(ENetPeer* p) {
    for (Peer& x : peers_)
        if (x.peer == p) return &x;
    return nullptr;
}

const NetPlayer* NetGame::player(uint8_t id) const {
    for (const NetPlayer& p : lobby_.players)
        if (p.id == id) return &p;
    return nullptr;
}

int NetGame::racer_of(uint8_t player_id) const {
    for (size_t i = 0; i < start_.racers.size(); ++i)
        if (start_.racers[i].player_id == player_id) return static_cast<int>(i);
    return -1;
}

bool NetGame::racer_present(int racer) const {
    return racer >= 0 && static_cast<size_t>(racer) < present_.size() &&
           present_[static_cast<size_t>(racer)] != 0;
}

void NetGame::service(uint32_t now_ms) {
    now_ms_ = now_ms;
    ENetEvent ev;
    while (host_ && enet_host_service(host_, &ev, 0) > 0) {
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (role_ == Role::Host) {
                enet_peer_timeout(ev.peer, 0, kTimeoutMin, kTimeoutMax);
                never_throttle(ev.peer);
                peers_.push_back(Peer{ev.peer, 0, false, InputLink()});
            } else {
                // Once connected: the setting is a command sent to the other end.
                never_throttle(ev.peer);
                NetWriter w;
                w.u8(static_cast<uint8_t>(NetMsg::Hello));
                w.u16(kProtocolVersion);
                write_look(w, my_look_);
                send(server_, w);
            }
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            on_receive(ev.peer, ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            on_disconnect(ev.peer);
            break;
        default:
            break;
        }
    }
    if (!host_) return;
    if (role_ == Role::Host) {
        fill_departed();
        GameAnnounce about;
        about.game_port = port_;
        about.host_name = my_look_.name;
        about.level = lobby_.settings.level;
        about.players = static_cast<uint8_t>(lobby_.players.size());
        about.max_players = lobby_.settings.max_players;
        about.phase = lobby_.phase;
        responder_.service(about);
    }
    // After the receiving: what the host just got goes on to the others in this same frame.
    send_inputs(now_ms);
    enet_host_flush(host_);
}

void NetGame::flush() {
    if (!host_) return;
    if (inputs_dirty_) send_inputs(now_ms_);
    enet_host_flush(host_);
}

void NetGame::on_receive(ENetPeer* from, const uint8_t* data, size_t size) {
    if (size < 1) return;
    NetReader r(data + 1, size - 1);
    const NetMsg type = static_cast<NetMsg>(data[0]);
    if (role_ == Role::Host) {
        Peer* p = find_peer(from);
        if (p) host_receive(*p, type, r);
    } else {
        client_receive(type, r);
    }
}

void NetGame::on_disconnect(ENetPeer* peer) {
    if (role_ == Role::Client) {
        error_ = welcomed_ ? "THE HOST HAS LEFT" : (error_.empty() ? "NO GAME AT THAT ADDRESS" : error_);
        std::printf("[net] %s\n", error_.c_str());
        std::fflush(stdout);
        close_host();
        ++revision_;
        return;
    }
    Peer* p = find_peer(peer);
    if (!p) return;
    const bool welcomed = p->welcomed;
    const uint8_t id = p->id;
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [&](const Peer& x) { return x.peer == peer; }),
                 peers_.end());
    if (!welcomed) return;
    std::printf("[net] player %u left\n", id);
    std::fflush(stdout);
    const int r = racer_of(id);
    if (lobby_.phase == NetPhase::Racing && r >= 0) {
        present_[static_cast<size_t>(r)] = 0;
        maybe_go();   // it will never load: do not wait for it
    }
    lobby_.players.erase(std::remove_if(lobby_.players.begin(), lobby_.players.end(),
                                        [&](const NetPlayer& x) { return x.id == id; }),
                         lobby_.players.end());
    lobby_changed();
    maybe_start_race();
}

void NetGame::lobby_changed() {
    ++revision_;
    if (role_ != Role::Host) return;
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::Lobby));
    write_lobby(w, lobby_);
    send_all(w);
}

// --- the host's side --------------------------------------------------------------------------

void NetGame::host_receive(Peer& from, NetMsg type, NetReader& r) {
    if (!from.welcomed && type != NetMsg::Hello) return;
    NetPlayer* pl = nullptr;
    for (NetPlayer& x : lobby_.players)
        if (x.id == from.id) pl = &x;
    switch (type) {
    case NetMsg::Hello: {
        if (from.welcomed) return;
        const uint16_t version = r.u16();
        NetLook look = read_look(r);
        if (!r.ok()) return;
        NetRefusal no = NetRefusal::None;
        if (version != kProtocolVersion) no = NetRefusal::Version;
        else if (lobby_.phase != NetPhase::Lobby) no = NetRefusal::Racing;
        else if (lobby_.players.size() >= lobby_.settings.max_players) no = NetRefusal::Full;
        if (no != NetRefusal::None) {
            NetWriter w;
            w.u8(static_cast<uint8_t>(NetMsg::Refuse));
            w.u8(static_cast<uint8_t>(no));
            send(from.peer, w);
            enet_peer_disconnect_later(from.peer, 0);
            return;
        }
        from.id = next_id_++;
        from.welcomed = true;
        if (look.name.empty()) look.name = "PLAYER " + std::to_string(from.id + 1);
        lobby_.players.push_back(NetPlayer{from.id, look, false, -1});
        NetWriter w;
        w.u8(static_cast<uint8_t>(NetMsg::Welcome));
        w.u8(from.id);
        send(from.peer, w);
        std::printf("[net] player %u (%s) joined\n", from.id, look.name.c_str());
        std::fflush(stdout);
        lobby_changed();
        return;
    }
    case NetMsg::SetLook: {
        NetLook look = read_look(r);
        if (!r.ok() || !pl) return;
        if (look.name.empty()) look.name = pl->look.name;
        pl->look = look;
        lobby_changed();
        return;
    }
    case NetMsg::SetReady: {
        const bool ready = r.u8() != 0;
        if (!r.ok() || !pl || lobby_.phase != NetPhase::Lobby) return;
        pl->ready = ready;
        lobby_changed();
        return;
    }
    case NetMsg::SetVehicle: {
        const int16_t v = static_cast<int16_t>(r.u8());
        if (!r.ok() || !pl || lobby_.phase != NetPhase::ChoosingVehicles) return;
        if (v < 0 || static_cast<size_t>(v) >= cars_.size()) return;
        pl->vehicle = v;
        lobby_changed();
        maybe_start_race();
        return;
    }
    case NetMsg::Loaded: {
        const uint32_t id = r.u32();
        const int racer = racer_of(from.id);
        if (!r.ok() || id != start_.race_id || racer < 0) return;
        loaded_[static_cast<size_t>(racer)] = 1;
        maybe_go();
        return;
    }
    case NetMsg::Inputs:
        // A machine only ever speaks for its own racer; the relay is send_inputs().
        receive_inputs(from.link, racer_of(from.id), r);
        return;
    case NetMsg::Checksum: {
        (void)r.u8();
        const uint32_t tick = r.u32();
        const uint32_t hash = r.u32();
        if (r.ok()) compare_checksum(tick, hash);
        return;
    }
    default:
        return;
    }
}

void NetGame::set_settings(const NetSettings& s, std::vector<std::string> cars) {
    if (role_ != Role::Host) return;
    cars_ = std::move(cars);
    if (lobby_.phase != NetPhase::Lobby) return;
    const bool new_level = s.level != lobby_.settings.level;
    lobby_.settings = s;
    if (lobby_.settings.max_players > kNetMaxPlayers) lobby_.settings.max_players = kNetMaxPlayers;
    if (lobby_.settings.max_players < 2) lobby_.settings.max_players = 2;
    if (lobby_.settings.races < 1) lobby_.settings.races = 1;
    if (lobby_.settings.races > kNetMaxSeries) lobby_.settings.races = kNetMaxSeries;
    // Ready for one circuit is not ready for another.
    if (new_level)
        for (NetPlayer& p : lobby_.players) p.ready = false;
    lobby_changed();
}

bool NetGame::everyone_ready() const {
    for (const NetPlayer& p : lobby_.players)
        if (p.id != 0 && !p.ready) return false;   // the host's own START is its "ready"
    return true;
}

bool NetGame::begin_vehicle_choice() {
    if (role_ != Role::Host || lobby_.phase != NetPhase::Lobby || !everyone_ready()) return false;
    if (cars_.empty()) return false;
    lobby_.phase = NetPhase::ChoosingVehicles;
    for (NetPlayer& p : lobby_.players) p.vehicle = -1;
    // The first race of a series - after the last one of the previous series, or the very
    // first: everyone back to zero.
    if (lobby_.series_done == 0 || lobby_.series_over()) {
        lobby_.series_done = 0;
        for (NetPlayer& p : lobby_.players) p.points = 0;
    }
    lobby_changed();
    return true;
}

void NetGame::record_race(const std::vector<std::pair<uint8_t, int>>& places) {
    if (role_ != Role::Host) return;
    for (const auto& pp : places)
        for (NetPlayer& p : lobby_.players)
            if (p.id == pp.first) p.points = static_cast<uint16_t>(p.points + net_series_points(pp.second));
    if (lobby_.series_done < lobby_.settings.races) ++lobby_.series_done;
    lobby_changed();
}

void NetGame::back_to_lobby() {
    go_ = false;
    start_pending_ = false;
    if (role_ != Role::Host) return;
    lobby_.phase = NetPhase::Lobby;
    for (NetPlayer& p : lobby_.players) {
        p.ready = false;
        p.vehicle = -1;
    }
    lobby_changed();
}

void NetGame::maybe_start_race() {
    if (role_ != Role::Host || lobby_.phase != NetPhase::ChoosingVehicles) return;
    if (lobby_.players.empty()) return;
    for (const NetPlayer& p : lobby_.players)
        if (p.vehicle < 0) return;
    NetRaceStart s;
    s.race_id = next_race_id_++;
    s.settings = lobby_.settings;
    s.delay = kNetInputDelay;
    s.rules = rules_;
    // The places turn by one every race, as the original's own network mode does
    // (`(race_number + i) % players`).
    const size_t n = lobby_.players.size();
    for (size_t i = 0; i < n; ++i) {
        const NetPlayer& p = lobby_.players[i];
        NetRacer x;
        x.player_id = p.id;
        x.grid = static_cast<uint8_t>((i + s.race_id) % n);
        x.vehicle = cars_[static_cast<size_t>(p.vehicle)];
        x.look = p.look;
        s.racers.push_back(std::move(x));
    }
    start_ = s;
    lobby_.phase = NetPhase::Racing;
    lobby_changed();
    configure_race();
    start_pending_ = true;
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::Start));
    write_start(w, start_);
    send_all(w);
    std::printf("[net] race %u: %s, %zu racer(s)\n", start_.race_id, start_.settings.level.c_str(),
                start_.racers.size());
    std::fflush(stdout);
}

void NetGame::configure_race() {
    const size_t n = start_.racers.size();
    local_racer_ = racer_of(local_id_);
    inputs_.configure(static_cast<int>(n), start_.delay);
    loaded_.assign(n, 0);
    present_.assign(n, 1);
    history_.assign(n, std::vector<uint16_t>(kHistory, 0));
    history_tick_.assign(n, std::vector<uint32_t>(kHistory, 0));
    contig_.assign(n, 0);
    for (size_t p = 0; p < n; ++p)
        for (uint32_t t = 1; t <= start_.delay; ++t) hold_input(static_cast<int>(p), t, 0);
    server_link_.reset(n, start_.delay);
    for (Peer& p : peers_) p.link.reset(n, start_.delay);
    last_local_tick_ = start_.delay;
    last_resync_ = 0;
    inputs_dirty_ = false;
    go_ = false;
    checks_.clear();
    desync_tick_ = 0;
    resync_tick_ = check_floor_ = snapshot_tick_ = resyncs_ = 0;
    snapshot_.clear();
    ++revision_;
}

void NetGame::race_loaded() {
    if (role_ == Role::Host) {
        if (local_racer_ >= 0) loaded_[static_cast<size_t>(local_racer_)] = 1;
        maybe_go();
        return;
    }
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::Loaded));
    w.u32(start_.race_id);
    send(server_, w);
}

void NetGame::maybe_go() {
    if (role_ != Role::Host || go_ || lobby_.phase != NetPhase::Racing) return;
    for (size_t i = 0; i < loaded_.size(); ++i)
        if (!loaded_[i] && present_[i]) return;
    go_ = true;
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::Go));
    w.u32(start_.race_id);
    send_all(w);
    std::printf("[net] race %u: everyone loaded, go\n", start_.race_id);
    std::fflush(stdout);
}

void NetGame::send_input(uint32_t tick, uint8_t bits) {
    if (local_racer_ < 0 || !race_configured()) return;
    hold_input(local_racer_, tick, bits);
    if (tick > last_local_tick_) last_local_tick_ = tick;
    ++stats_.inputs_sent;
    inputs_dirty_ = true;   // leaves at the flush() that ends this frame's ticks
}

bool NetGame::hold_input(int racer, uint32_t tick, uint8_t bits) {
    if (racer < 0 || static_cast<size_t>(racer) >= contig_.size() || tick == 0) return false;
    const size_t r = static_cast<size_t>(racer);
    // Nothing older than the gapless run matters any more, nor anything beyond the window.
    if (tick <= contig_[r] || tick > contig_[r] + kHistory) return false;
    const size_t row = tick % kHistory;
    if (history_tick_[r][row] == tick && history_[r][row] != 0) return false;
    history_tick_[r][row] = tick;
    history_[r][row] = static_cast<uint16_t>(0x100 | (bits & 0x3F));
    inputs_.submit(racer, tick, bits);
    uint32_t& c = contig_[r];
    while (history_tick_[r][(c + 1) % kHistory] == c + 1 && history_[r][(c + 1) % kHistory] != 0) ++c;
    return true;
}

// One INPUTS packet to one machine, when it needs one: something new, something to report, or
// something still unacknowledged since the last resend delay. It carries, per racer, every held
// input from what the other end acknowledged to the end of the gapless run.
void NetGame::send_inputs_to(ENetPeer* to, InputLink& link, int skip_racer, uint32_t now_ms) {
    const size_t n = contig_.size();
    bool fresh = false, pending = false;
    for (size_t r = 0; r < n; ++r) {
        if (static_cast<int>(r) == skip_racer) continue;
        const bool relay = role_ == Role::Host || static_cast<int>(r) == local_racer_;
        if (!relay) continue;
        if (contig_[r] > link.sent_upto[r]) fresh = true;
        if (contig_[r] > link.acked[r]) pending = true;
    }
    for (size_t r = 0; r < n; ++r)
        if (contig_[r] != link.told[r]) fresh = true;
    if (!fresh && !(pending && now_ms - link.last_ms >= kInputResendMs)) return;

    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::Inputs));
    w.u32(start_.race_id);
    w.u32(role_ == Role::Host ? last_resync_ : 0);
    w.u8(static_cast<uint8_t>(n));
    for (size_t r = 0; r < n; ++r) {
        w.u8(static_cast<uint8_t>(r));
        w.u32(contig_[r]);
        link.told[r] = contig_[r];
    }
    std::vector<size_t> runs;
    for (size_t r = 0; r < n; ++r) {
        if (static_cast<int>(r) == skip_racer) continue;
        if (role_ != Role::Host && static_cast<int>(r) != local_racer_) continue;
        if (contig_[r] > link.acked[r]) runs.push_back(r);
    }
    w.u8(static_cast<uint8_t>(runs.size()));
    for (size_t r : runs) {
        const uint32_t first = link.acked[r] + 1;
        const uint32_t count = std::min(contig_[r] - link.acked[r], kInputsPerRun);
        w.u8(static_cast<uint8_t>(r));
        w.u32(first);
        w.u8(static_cast<uint8_t>(count));
        for (uint32_t t = first; t < first + count; ++t)
            w.u8(static_cast<uint8_t>(history_[r][t % kHistory] & 0x3F));
        link.sent_upto[r] = std::max(link.sent_upto[r], first + count - 1);
    }
    link.last_ms = now_ms;
    if (input_loss_ > 0) {
        loss_seed_ = loss_seed_ * 1103515245u + 12345u;
        if (static_cast<int>((loss_seed_ >> 16) % 100) < input_loss_) return;   // "lost"
    }
    ENetPacket* p = enet_packet_create(w.bytes().data(), w.bytes().size(), 0);
    enet_peer_send(to, kInputChannel, p);
}

void NetGame::send_inputs(uint32_t now_ms) {
    inputs_dirty_ = false;
    if (lobby_.phase != NetPhase::Racing || !race_configured()) return;
    if (role_ == Role::Host) {
        for (Peer& p : peers_)
            if (p.welcomed) send_inputs_to(p.peer, p.link, racer_of(p.id), now_ms);
    } else if (server_ && welcomed_) {
        send_inputs_to(server_, server_link_, -1, now_ms);
    }
}

// `only_racer`: the host takes from a client that client's racer and nothing else.
void NetGame::receive_inputs(InputLink& link, int only_racer, NetReader& r) {
    const uint32_t race_id = r.u32();
    const uint32_t resync = r.u32();
    // Before its START, or from the race before: dropped, and sent again until acknowledged.
    if (!r.ok() || !race_configured() || race_id != start_.race_id) return;
    const size_t n = contig_.size();
    const int acks = r.u8();
    for (int i = 0; i < acks; ++i) {
        const size_t racer = r.u8();
        const uint32_t upto = r.u32();
        if (!r.ok() || racer >= n) return;
        if (upto > link.acked[racer]) link.acked[racer] = upto;
    }
    const int runs = r.u8();
    for (int i = 0; i < runs; ++i) {
        const int racer = r.u8();
        const uint32_t first = r.u32();
        const int count = r.u8();
        if (!r.ok() || racer < 0 || static_cast<size_t>(racer) >= n) return;
        const bool mine = role_ == Role::Host ? racer == only_racer : racer != local_racer_;
        for (int k = 0; k < count; ++k) {
            const uint8_t bits = r.u8();
            if (!r.ok()) return;
            if (!mine || (bits & 0xC0) != 0) continue;
            if (hold_input(racer, first + static_cast<uint32_t>(k), bits)) ++stats_.inputs_received;
        }
    }
    // ⚠️ THE RESYNC, SEEN ON THIS CHANNEL TOO. The announcement travels reliable on channel 0,
    // but the inputs that would let this machine run past R can arrive first on channel 1: every
    // host packet names the last R, and a client that has not applied it stops there.
    if (role_ != Role::Host && resync > last_resync_ && resync_tick_ == 0) {
        resync_tick_ = resync;
        ++revision_;
    }
}

// A racer whose machine has gone keeps its car on the track with nothing pressed, and it is
// the HOST that says so, through the same INPUT messages as everyone else - so every machine
// sees exactly the same inputs for it, in the same order, after the last one it really sent.
void NetGame::fill_departed() {
    if (lobby_.phase != NetPhase::Racing || !go_ || !race_configured()) return;
    // From the end of what the host holds without a gap: an input it held beyond a gap is kept.
    for (size_t r = 0; r < present_.size(); ++r) {
        if (present_[r]) continue;
        for (uint32_t t = contig_[r] + 1; t <= last_local_tick_; ++t) hold_input(static_cast<int>(r), t, 0);
    }
}

void NetGame::report_checksum(uint32_t tick, uint32_t hash) {
    if (role_ == Role::Host) {
        compare_checksum(tick, hash);
        return;
    }
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::Checksum));
    w.u8(static_cast<uint8_t>(local_racer_ < 0 ? 0 : local_racer_));
    w.u32(tick);
    w.u32(hash);
    send(server_, w);
}

void NetGame::compare_checksum(uint32_t tick, uint32_t hash) {
    // Reports from before the last resync describe a race that has since been replaced.
    if (tick <= check_floor_) return;
    auto it = checks_.find(tick);
    if (it == checks_.end()) {
        checks_.emplace(tick, hash);
        // Forget what is long settled: every machine has reported it by now.
        while (!checks_.empty() && checks_.begin()->first + 4096 < tick) checks_.erase(checks_.begin());
        return;
    }
    ++stats_.checksums_compared;
    if (it->second == hash || desync_tick_ != 0) return;
    desync_tick_ = tick;
    std::printf("[net] DESYNC at tick %u\n", tick);
    std::fflush(stdout);
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::Desync));
    w.u32(tick);
    send_all(w);
    // And the cure, straight away. `last_local_tick_` is the furthest tick the host has sent an
    // input for, so the furthest any client can have run; R is the one after, and its input
    // leaves after this message on the same ordered channel.
    if (resync_tick_ == 0) {
        resync_tick_ = last_local_tick_ + 1;
        last_resync_ = resync_tick_;
        NetWriter a;
        a.u8(static_cast<uint8_t>(NetMsg::ResyncAt));
        a.u32(resync_tick_);
        send_all(a);
        std::printf("[net] resync at tick %u\n", resync_tick_);
        std::fflush(stdout);
    }
    ++revision_;
}

void NetGame::send_snapshot(uint32_t tick, const std::vector<uint8_t>& race) {
    if (role_ != Role::Host || tick == 0 || tick != resync_tick_) return;
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::Snapshot));
    w.u32(tick);
    w.blob(race);
    send_all(w);
    // Over, as far as the host is concerned: its own race IS the reference. Everything reported
    // for ticks up to R compared a race that no longer exists.
    check_floor_ = tick;
    checks_.clear();
    resync_tick_ = 0;
    desync_tick_ = 0;
    ++resyncs_;
    ++revision_;
    std::printf("[net] snapshot of tick %u sent, %zu bytes\n", tick, race.size());
    std::fflush(stdout);
}

void NetGame::snapshot_applied(bool ok) {
    if (role_ == Role::Host) return;
    if (ok) {
        desync_tick_ = 0;
        ++resyncs_;
    }
    if (resync_tick_ > last_resync_) last_resync_ = resync_tick_;
    resync_tick_ = 0;
    snapshot_tick_ = 0;
    snapshot_.clear();
    ++revision_;
}

// --- everyone ---------------------------------------------------------------------------------

void NetGame::set_look(const NetLook& look) {
    my_look_ = look;
    if (role_ == Role::Host) {
        if (!lobby_.players.empty()) lobby_.players[0].look = look;
        lobby_changed();
        return;
    }
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::SetLook));
    write_look(w, look);
    send(server_, w);
}

void NetGame::set_ready(bool ready) {
    if (role_ == Role::Host) {
        if (!lobby_.players.empty()) lobby_.players[0].ready = ready;
        lobby_changed();
        return;
    }
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::SetReady));
    w.u8(ready ? 1 : 0);
    send(server_, w);
}

void NetGame::set_vehicle(int16_t index) {
    if (role_ == Role::Host) {
        if (lobby_.phase != NetPhase::ChoosingVehicles || lobby_.players.empty()) return;
        if (index < 0 || static_cast<size_t>(index) >= cars_.size()) return;
        lobby_.players[0].vehicle = index;
        lobby_changed();
        maybe_start_race();
        return;
    }
    NetWriter w;
    w.u8(static_cast<uint8_t>(NetMsg::SetVehicle));
    w.u8(static_cast<uint8_t>(index));
    send(server_, w);
}

// --- the client's side ------------------------------------------------------------------------

void NetGame::client_receive(NetMsg type, NetReader& r) {
    switch (type) {
    case NetMsg::Welcome: {
        const uint8_t id = r.u8();
        if (!r.ok()) return;
        local_id_ = id;
        welcomed_ = true;
        std::printf("[net] welcomed as player %u\n", id);
        std::fflush(stdout);
        ++revision_;
        return;
    }
    case NetMsg::Refuse: {
        const NetRefusal why = static_cast<NetRefusal>(r.u8());
        error_ = refusal_text(why);
        std::printf("[net] refused: %s\n", error_.c_str());
        std::fflush(stdout);
        ++revision_;
        return;   // the host disconnects us next; on_disconnect closes
    }
    case NetMsg::Lobby: {
        NetLobbyState s;
        if (!read_lobby(r, s)) return;
        const bool left_race = lobby_.phase == NetPhase::Racing && s.phase == NetPhase::Lobby;
        lobby_ = std::move(s);
        if (left_race) {
            go_ = false;
            start_pending_ = false;
        }
        ++revision_;
        return;
    }
    case NetMsg::Start: {
        NetRaceStart s;
        if (!read_start(r, s)) return;
        start_ = std::move(s);
        configure_race();
        start_pending_ = true;
        return;
    }
    case NetMsg::Go: {
        const uint32_t id = r.u32();
        if (r.ok() && id == start_.race_id) go_ = true;
        return;
    }
    case NetMsg::Inputs:
        receive_inputs(server_link_, -1, r);
        return;
    case NetMsg::Desync: {
        const uint32_t tick = r.u32();
        if (r.ok() && desync_tick_ == 0) desync_tick_ = tick;
        ++revision_;
        return;
    }
    case NetMsg::ResyncAt: {
        const uint32_t tick = r.u32();
        if (r.ok()) resync_tick_ = tick;
        ++revision_;
        return;
    }
    case NetMsg::Snapshot: {
        const uint32_t tick = r.u32();
        std::vector<uint8_t> race = r.blob();
        if (!r.ok()) return;
        snapshot_ = std::move(race);
        snapshot_tick_ = tick;
        resync_tick_ = tick;   // in case the announcement was somehow missed
        ++revision_;
        return;
    }
    default:
        return;
    }
}

} // namespace bigred
