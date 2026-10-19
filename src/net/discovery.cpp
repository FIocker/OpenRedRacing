#include "discovery.h"

#include <enet/enet.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bigred {
namespace {

// Four bytes of magic in front of both packets: a stray datagram on the port is ignored.
constexpr uint8_t kQueryMagic[4] = {'O', 'B', 'R', 'Q'};
constexpr uint8_t kAnswerMagic[4] = {'O', 'B', 'R', 'A'};
constexpr uint32_t kQueryEveryMs = 1000;
constexpr uint32_t kForgetAfterMs = 3500;

ENetSocket as_socket(uintptr_t s) { return static_cast<ENetSocket>(s); }

bool starts_with(const uint8_t* data, size_t size, const uint8_t (&magic)[4]) {
    return size >= 4 && std::memcmp(data, magic, 4) == 0;
}

ENetAddress loopback(uint16_t port) {
    ENetAddress a{};
    enet_address_set_host_ip(&a, "127.0.0.1");
    a.port = port;
    return a;
}

void send_to(ENetSocket s, const ENetAddress& to, const std::vector<uint8_t>& bytes) {
    ENetBuffer buf;
    buf.data = const_cast<uint8_t*>(bytes.data());
    buf.dataLength = bytes.size();
    enet_socket_send(s, &to, &buf, 1);
}

// Reads what is waiting, at most `max` datagrams. ⚠️ enet_socket_receive answers 0 both for
// "nothing waiting" and for Windows' WSAECONNRESET - the echo of an earlier send to a port
// nobody listens on - so a 0 ends this frame's reading rather than the conversation.
template <typename F>
void drain(ENetSocket s, F&& on_datagram, int max = 32) {
    uint8_t data[512];
    for (int i = 0; i < max; ++i) {
        ENetAddress from{};
        ENetBuffer buf;
        buf.data = data;
        buf.dataLength = sizeof(data);
        const int n = enet_socket_receive(s, &from, &buf, 1);
        if (n <= 0) return;
        on_datagram(from, data, static_cast<size_t>(n));
    }
}

int g_net_users = 0;

} // namespace

void net_acquire() {
    if (g_net_users++ == 0) enet_initialize();
}

void net_release() {
    if (--g_net_users == 0) enet_deinitialize();
}

bool DiscoveryResponder::open(bool loopback_only) {
    close();
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) return false;
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    // Several hosts on one machine share the port; each answers the broadcasts it hears.
    enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);
    enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
    ENetAddress a{};
    if (loopback_only) a = loopback(kDiscoveryPort);
    else {
        a.host = ENET_HOST_ANY;
        a.port = kDiscoveryPort;
    }
    if (enet_socket_bind(s, &a) < 0) {
        enet_socket_destroy(s);
        return false;
    }
    socket_ = static_cast<uintptr_t>(s);
    return true;
}

void DiscoveryResponder::close() {
    if (socket_ != kNoSocket) enet_socket_destroy(as_socket(socket_));
    socket_ = kNoSocket;
}

void DiscoveryResponder::service(const GameAnnounce& about) {
    if (socket_ == kNoSocket) return;
    const ENetSocket s = as_socket(socket_);
    drain(s, [&](const ENetAddress& from, const uint8_t* data, size_t size) {
        if (!starts_with(data, size, kQueryMagic)) return;
        NetReader r(data + 4, size - 4);
        if (r.u16() != kProtocolVersion || !r.ok()) return;   // another version: stay silent
        NetWriter w;
        for (uint8_t m : kAnswerMagic) w.u8(m);
        w.u16(kProtocolVersion);
        w.u16(about.game_port);
        w.str(about.host_name);
        w.str(about.level);
        w.u8(about.players);
        w.u8(about.max_players);
        w.u8(static_cast<uint8_t>(about.phase));
        send_to(s, from, w.bytes());
    });
}

bool DiscoveryBrowser::open(bool loopback_only) {
    close();
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) return false;
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
    ENetAddress a{};
    if (loopback_only) a = loopback(0);
    else {
        a.host = ENET_HOST_ANY;
        a.port = 0;   // any free port: the replies come back to it
    }
    if (enet_socket_bind(s, &a) < 0) {
        enet_socket_destroy(s);
        return false;
    }
    socket_ = static_cast<uintptr_t>(s);
    loopback_only_ = loopback_only;
    queried_ = false;
    games_.clear();
    return true;
}

void DiscoveryBrowser::close() {
    if (socket_ != kNoSocket) enet_socket_destroy(as_socket(socket_));
    socket_ = kNoSocket;
}

void DiscoveryBrowser::service(uint32_t now_ms) {
    if (socket_ == kNoSocket) return;
    const ENetSocket s = as_socket(socket_);
    if (!queried_ || now_ms - last_query_ms_ >= kQueryEveryMs) {
        queried_ = true;
        last_query_ms_ = now_ms;
        NetWriter w;
        for (uint8_t m : kQueryMagic) w.u8(m);
        w.u16(kProtocolVersion);
        // The loopback copy is what finds a host on this very machine even where the
        // interface does not loop broadcasts back.
        send_to(s, loopback(kDiscoveryPort), w.bytes());
        if (!loopback_only_) {
            ENetAddress b{};
            b.host = ENET_HOST_BROADCAST;
            b.port = kDiscoveryPort;
            send_to(s, b, w.bytes());
        }
    }
    drain(s, [&](const ENetAddress& from, const uint8_t* data, size_t size) {
        if (!starts_with(data, size, kAnswerMagic)) return;
        NetReader r(data + 4, size - 4);
        if (r.u16() != kProtocolVersion) return;
        GameAnnounce g;
        g.game_port = r.u16();
        g.host_name = r.str();
        g.level = r.str();
        g.players = r.u8();
        g.max_players = r.u8();
        const uint8_t phase = r.u8();
        if (!r.ok() || phase > static_cast<uint8_t>(NetPhase::Racing)) return;
        g.phase = static_cast<NetPhase>(phase);
        char ip[64] = {};
        enet_address_get_host_ip(&from, ip, sizeof(ip));
        g.address = ip;
        g.seen_ms = now_ms;
        // One entry per game. The same host heard twice - by loopback and by broadcast - is
        // one game when it is this machine: 127.0.0.1 and the LAN address are both itself.
        auto same = std::find_if(games_.begin(), games_.end(), [&](const GameAnnounce& o) {
            return o.game_port == g.game_port &&
                   (o.address == g.address ||
                    (o.host_name == g.host_name && (o.address == "127.0.0.1" ||
                                                    g.address == "127.0.0.1")));
        });
        if (same == games_.end()) {
            std::printf("[net] found \"%s\" at %s:%u\n", g.host_name.c_str(), g.address.c_str(),
                        g.game_port);
            std::fflush(stdout);
            games_.push_back(g);
        } else {
            // Keep the LAN address over the loopback one: it is the one that works from
            // elsewhere, and it works from here too.
            const std::string keep = (g.address == "127.0.0.1") ? same->address : g.address;
            *same = g;
            same->address = keep;
        }
    });
    games_.erase(std::remove_if(games_.begin(), games_.end(),
                                [&](const GameAnnounce& g) {
                                    return now_ms - g.seen_ms > kForgetAfterMs;
                                }),
                 games_.end());
}

std::string local_address() {
    char name[256] = {};
    if (gethostname(name, sizeof(name)) != 0) return {};
    ENetAddress a{};
    if (enet_address_set_host(&a, name) != 0) return {};
    char ip[64] = {};
    if (enet_address_get_host_ip(&a, ip, sizeof(ip)) != 0) return {};
    const std::string s = ip;
    return s.rfind("127.", 0) == 0 ? std::string() : s;
}

} // namespace bigred
