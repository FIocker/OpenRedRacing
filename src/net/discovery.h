// Finding the games on the local network, with no server.
//
//
// ⚠️ QUERY AND REPLY, NOT A BEACON THE CLIENTS LISTEN TO. The design first said "the host
// broadcasts, the clients listen". That breaks on ONE machine:
// several clients cannot all hear a datagram sent to a port they share - Windows hands a
// unicast to only one of the sockets bound to it. So the CLIENT asks, from its own ephemeral
// port, by broadcast AND by loopback, and every host answers straight back to that port. Every
// client hears every host, whether they are on one machine or twenty.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net_protocol.h"

namespace bigred {

// ENet's process-wide start-up (Winsock's, underneath), counted: every object that opens a
// socket holds one. ⚠️ THE DISCOVERY NEEDS IT TOO, not only NetGame: the join screen searches
// the network before any game object exists, and on Windows a socket made before WSAStartup
// simply fails - the list stayed empty for exactly that reason.
void net_acquire();
void net_release();

// What a host says about itself.
struct GameAnnounce {
    std::string address;      // dotted quad, as the reply came from
    uint16_t game_port = kGamePort;
    std::string host_name;
    std::string level;
    uint8_t players = 0, max_players = 0;
    NetPhase phase = NetPhase::Lobby;
    uint32_t seen_ms = 0;     // browser clock, for expiry
};

// Opened by a HOST. Answers the queries it hears. `loopback_only` binds 127.0.0.1 alone - the
// bench uses it so a test never opens a port to the network (and never raises the firewall's
// question).
class DiscoveryResponder {
public:
    DiscoveryResponder() { net_acquire(); }
    ~DiscoveryResponder() {
        close();
        net_release();
    }
    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;
    bool open(bool loopback_only);
    void close();
    // Answers every pending query with `about`.
    void service(const GameAnnounce& about);
    bool is_open() const { return socket_ != kNoSocket; }

private:
    static constexpr uintptr_t kNoSocket = ~static_cast<uintptr_t>(0);
    uintptr_t socket_ = kNoSocket;
};

// Opened by the JOIN screen. Asks every second, keeps what answered in the last 3.5 s.
class DiscoveryBrowser {
public:
    DiscoveryBrowser() { net_acquire(); }
    ~DiscoveryBrowser() {
        close();
        net_release();
    }
    DiscoveryBrowser(const DiscoveryBrowser&) = delete;
    DiscoveryBrowser& operator=(const DiscoveryBrowser&) = delete;
    bool open(bool loopback_only);
    void close();
    // `now_ms` is any monotonic millisecond clock - a display clock, never the simulation's.
    void service(uint32_t now_ms);
    const std::vector<GameAnnounce>& games() const { return games_; }

private:
    static constexpr uintptr_t kNoSocket = ~static_cast<uintptr_t>(0);
    uintptr_t socket_ = kNoSocket;
    bool loopback_only_ = false;
    uint32_t last_query_ms_ = 0;
    bool queried_ = false;
    std::vector<GameAnnounce> games_;
};

// This machine's address on the network, as the others would type it - the first one its name
// resolves to. Empty when there is none but the loopback.
std::string local_address();

} // namespace bigred
