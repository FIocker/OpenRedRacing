#include "net_protocol.h"

namespace bigred {

void NetWriter::str(const std::string& s) {
    const size_t n = s.size() > 255 ? 255 : s.size();
    u8(static_cast<uint8_t>(n));
    b_.insert(b_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

void NetWriter::blob(const std::vector<uint8_t>& b) {
    u32(static_cast<uint32_t>(b.size()));
    b_.insert(b_.end(), b.begin(), b.end());
}

std::vector<uint8_t> NetReader::blob() {
    const uint32_t n = u32();
    if (!ok_ || static_cast<size_t>(end_ - p_) < n) {
        ok_ = false;
        p_ = end_;
        return {};
    }
    std::vector<uint8_t> b(p_, p_ + n);
    p_ += n;
    return b;
}

uint8_t NetReader::u8() {
    if (p_ >= end_) {
        ok_ = false;
        return 0;
    }
    return *p_++;
}

uint16_t NetReader::u16() {
    const uint16_t lo = u8();
    return static_cast<uint16_t>(lo | (u8() << 8));
}

uint32_t NetReader::u32() {
    const uint32_t lo = u16();
    return lo | (static_cast<uint32_t>(u16()) << 16);
}

std::string NetReader::str() {
    const size_t n = u8();
    if (static_cast<size_t>(end_ - p_) < n) {
        ok_ = false;
        p_ = end_;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return s;
}

void write_look(NetWriter& w, const NetLook& l) {
    w.str(l.name);
    w.u8(l.colour);
    w.u8(l.logo);
    w.u8(l.damage);
    w.u8(l.character);
    w.u8(l.character_colour);
    w.blob(l.custom_logo.size() <= kNetCustomLogoMaxBytes ? l.custom_logo : std::vector<uint8_t>());
}

NetLook read_look(NetReader& r) {
    NetLook l;
    l.name = r.str();
    l.colour = r.u8();
    l.logo = r.u8();
    l.damage = r.u8();
    if (l.damage > 100) l.damage = 100;
    l.character = static_cast<uint8_t>(r.u8() % 6);
    l.character_colour = r.u8();
    l.custom_logo = r.blob();
    if (l.custom_logo.size() > kNetCustomLogoMaxBytes) l.custom_logo.clear();
    return l;
}

namespace {

void write_settings(NetWriter& w, const NetSettings& s) {
    w.str(s.level);
    w.u8(s.laps);
    w.u8(s.ghosts ? 1 : 0);
    w.u8(s.max_players);
    w.u8(s.races);
    w.u8(s.random_circuit ? 1 : 0);
}

NetSettings read_settings(NetReader& r) {
    NetSettings s;
    s.level = r.str();
    s.laps = r.u8();
    s.ghosts = r.u8() != 0;
    s.max_players = r.u8();
    s.races = r.u8();
    if (s.races < 1) s.races = 1;
    if (s.races > kNetMaxSeries) s.races = kNetMaxSeries;
    s.random_circuit = r.u8() != 0;
    return s;
}

} // namespace

void write_lobby(NetWriter& w, const NetLobbyState& s) {
    w.u8(static_cast<uint8_t>(s.phase));
    write_settings(w, s.settings);
    w.u8(s.series_done);
    w.u8(static_cast<uint8_t>(s.players.size()));
    for (const NetPlayer& p : s.players) {
        w.u8(p.id);
        write_look(w, p.look);
        w.u8(p.ready ? 1 : 0);
        w.u16(static_cast<uint16_t>(p.vehicle));
        w.u16(p.points);
    }
}

bool read_lobby(NetReader& r, NetLobbyState& s) {
    NetLobbyState out;
    const uint8_t phase = r.u8();
    if (phase > static_cast<uint8_t>(NetPhase::Racing)) return false;
    out.phase = static_cast<NetPhase>(phase);
    out.settings = read_settings(r);
    out.series_done = r.u8();
    const int n = r.u8();
    for (int i = 0; i < n && r.ok(); ++i) {
        NetPlayer p;
        p.id = r.u8();
        p.look = read_look(r);
        p.ready = r.u8() != 0;
        p.vehicle = static_cast<int16_t>(r.u16());
        p.points = r.u16();
        out.players.push_back(std::move(p));
    }
    if (!r.ok()) return false;
    s = std::move(out);
    return true;
}

void write_start(NetWriter& w, const NetRaceStart& s) {
    w.u32(s.race_id);
    write_settings(w, s.settings);
    w.u8(s.delay);
    w.u8(static_cast<uint8_t>(s.racers.size()));
    for (const NetRacer& r : s.racers) {
        w.u8(r.player_id);
        w.u8(r.grid);
        w.str(r.vehicle);
        write_look(w, r.look);
    }
    int32_t words[RaceRules::kWords];
    s.rules.to_words(words);
    for (int32_t v : words) w.u32(static_cast<uint32_t>(v));
}

bool read_start(NetReader& r, NetRaceStart& s) {
    NetRaceStart out;
    out.race_id = r.u32();
    out.settings = read_settings(r);
    out.delay = r.u8();
    const int n = r.u8();
    for (int i = 0; i < n && r.ok(); ++i) {
        NetRacer x;
        x.player_id = r.u8();
        x.grid = r.u8();
        x.vehicle = r.str();
        x.look = read_look(r);
        out.racers.push_back(std::move(x));
    }
    int32_t words[RaceRules::kWords];
    for (int32_t& v : words) v = static_cast<int32_t>(r.u32());
    out.rules.from_words(words);
    if (!r.ok() || out.racers.empty()) return false;
    s = std::move(out);
    return true;
}

} // namespace bigred
