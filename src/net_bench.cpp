// net_bench - the network game, tested with no window, no GL and no second computer.
//
//
//   net_bench [game_dir|-] [LEVEL]                 everything, in ONE process, over 127.0.0.1
//   net_bench [game_dir|-] [LEVEL] host N TICKS    one machine of a multi-process test: hosts,
//   net_bench [game_dir|-] [LEVEL] join TICKS      waits for N players, races TICKS ticks
//   net_bench - lag [PLAYERS] [DELAY] [redundant]  the LATENCY MODEL: a table, ping 0-300 ms
//   net_bench - LEVEL flips [N] [TICKS] [VEH]     the autodrive race, and every car that goes over
//
// Every test stays on the loopback interface: nothing is ever opened to the network, so the
// bench never raises the firewall's question.
//
// What the in-process run checks, in order:
//   1. the wire format reads back what it wrote, and refuses a short packet;
//   2. DISCOVERY - a browser finds the host;
//   3. the LOBBY - two clients join, set themselves ready, a fourth is turned away when full;
//   4. the VEHICLE CHOICE and START - every machine receives the same race;
//   5. THE RACE - three RaceSessions, one per machine, each fed only by the network, must
//      agree with a REFERENCE session fed the same inputs directly, at EVERY tick. The
//      machines are serviced in a shuffled order and one of them skips frames;
//   6. a DESYNC is detected AND REPAIRED: one machine's car is nudged by one unit, the host
//      must say so, send its race at the agreed tick, and all three must agree again after;
//   7. a racer LEAVES mid-race: the others must go on, and still agree with each other.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "net/discovery.h"
#include "net/net_game.h"
#include "net/net_protocol.h"
#include "scene/level_scene.h"
#include "scene/race_level.h"
#include "sim/lockstep.h"
#include "sim/race_session.h"
#include "sim/snapshot.h"

namespace {

using namespace bigred;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

// Everyone's driving: full throttle, steering in bursts from a per-racer hash, a boost now and
// then. A pure function of (racer, tick), so the reference session can be fed the same.
uint8_t script(int racer, uint32_t tick) {
    VehicleInput in;
    in.accel = true;
    uint32_t s = 2654435761u * static_cast<uint32_t>(racer + 1) + (tick / 20) * 40503u;
    s ^= s >> 13;
    s *= 2246822519u;
    s ^= s >> 16;
    in.left = (s % 3) == 0;
    in.right = (s % 3) == 1;
    in.nitro = (s % 17) == 0;
    in.recover = (s % 211) == 0;
    return pack_input(in);
}

uint32_t clock_ms() { return SDL_GetTicks(); }

// One machine's race: its own world, built from the START it was sent.
struct Machine {
    std::unique_ptr<NetGame> net;
    std::string name;
    RaceField field;
    SceneryState world;
    std::unique_ptr<RaceSession> session;
    std::vector<VehicleInput> inputs;   // per session racer
    std::vector<uint32_t> hashes;       // per tick, from tick 1
    uint32_t sent_upto = 0;             // the last tick this machine's input was sent for
    bool built = false;
    int resyncs_loaded = 0;             // host snapshots this machine has loaded
};

std::vector<FieldHuman> humans_of(const NetRaceStart& s) {
    std::vector<FieldHuman> h;
    for (const NetRacer& r : s.racers) h.push_back(FieldHuman{r.grid, r.vehicle, r.look.damage});
    return h;
}

bool build_machine(Machine& m, const std::string& game_dir, const LevelScene& scene,
                   const RaceLevel& level) {
    const NetRaceStart& s = m.net->race();
    if (!build_race_field(game_dir, scene, level, humans_of(s), s.settings.ghosts, m.field))
        return false;
    m.world = SceneryState();
    m.world.resize(scene.instances.size());
    m.session = std::make_unique<RaceSession>();
    m.session->rules = s.rules;   // the host's, as the game does
    m.session->configure(level.terrain, &m.world, level.gates,
                         s.settings.laps ? s.settings.laps : level.laps, level.countdown_ticks,
                         level.lines, level.grid, level.nitros, m.field.racers, level.origin,
                         level.rates);
    m.inputs.assign(static_cast<size_t>(m.session->racer_count()), VehicleInput());
    m.hashes.clear();
    m.sent_upto = s.delay;
    m.built = true;
    m.net->take_start();
    m.net->race_loaded();
    return true;
}

// The resync, exactly as RaceScreen::handle_resync does it: at tick R the host sends its race,
// a client waits for it and loads it. False while a client has to wait.
bool resync_machine(Machine& m) {
    const uint32_t r = m.net->resync_tick();
    if (r == 0 || m.session->ticks() != r) return true;
    if (m.net->is_host()) {
        m.net->send_snapshot(r, save_race(*m.session));
        return true;
    }
    const std::vector<uint8_t>* snap = m.net->snapshot_for(r);
    if (!snap) return false;
    const bool ok = load_race(*m.session, snap->data(), snap->size());
    m.net->snapshot_applied(ok);
    if (ok) ++m.resyncs_loaded;
    return true;
}

// One frame of one machine: send what is due, then run every tick whose inputs are all in, at
// most `max_ticks` of them. Returns the ticks run.
int step_machine(Machine& m, uint32_t until, int max_ticks) {
    if (!m.built || !m.net->race_go() || !m.net->error().empty()) return 0;
    LockstepInputs& lk = m.net->inputs();
    const int me = m.net->local_racer();
    int ran = 0;
    while (ran < max_ticks && m.session->ticks() < until) {
        if (!resync_machine(m)) break;
        const uint32_t next = m.session->ticks() + 1;
        const uint32_t due = lk.schedule(next);
        if (m.sent_upto < due) {
            m.sent_upto = due;
            m.net->send_input(due, script(me, due));
        }
        if (!lk.ready(next)) break;
        std::vector<VehicleInput> per(static_cast<size_t>(lk.players()));
        lk.inputs_for(next, per.data());
        for (size_t h = 0; h < per.size(); ++h)
            m.inputs[static_cast<size_t>(m.field.session_of[h])] = per[h];
        m.session->tick(m.inputs.data());
        lk.release_before(next);
        const uint32_t hash = m.session->checksum();
        m.hashes.push_back(hash);
        if (next % kNetChecksumEvery == 0) m.net->report_checksum(next, hash);
        ++ran;
    }
    resync_machine(m);
    return ran;
}

// The reference: the same field, the same inputs, no network at all.
std::vector<uint32_t> reference_run(const std::string& game_dir, const LevelScene& scene,
                                    const RaceLevel& level, const NetRaceStart& s, uint32_t ticks) {
    RaceField field;
    build_race_field(game_dir, scene, level, humans_of(s), s.settings.ghosts, field);
    SceneryState world;
    world.resize(scene.instances.size());
    RaceSession session;
    session.rules = s.rules;
    session.configure(level.terrain, &world, level.gates,
                      s.settings.laps ? s.settings.laps : level.laps, level.countdown_ticks,
                      level.lines, level.grid, level.nitros, field.racers, level.origin,
                      level.rates);
    std::vector<VehicleInput> in(static_cast<size_t>(session.racer_count()));
    std::vector<uint32_t> out;
    for (uint32_t t = 1; t <= ticks; ++t) {
        for (size_t h = 0; h < s.racers.size(); ++h)
            in[static_cast<size_t>(field.session_of[h])] =
                t <= s.delay ? VehicleInput() : unpack_input(script(static_cast<int>(h), t));
        session.tick(in.data());
        out.push_back(session.checksum());
    }
    return out;
}

void pump(std::vector<Machine*>& ms, int rounds = 1) {
    for (int k = 0; k < rounds; ++k) {
        for (Machine* m : ms) m->net->service(clock_ms());
        SDL_Delay(1);
    }
}

template <typename F>
bool pump_until(std::vector<Machine*>& ms, F&& done, int max_ms = 5000) {
    const uint32_t t0 = clock_ms();
    while (clock_ms() - t0 < static_cast<uint32_t>(max_ms)) {
        pump(ms);
        if (done()) return true;
    }
    return false;
}

void test_wire() {
    std::printf("\n1. LE FORMAT\n");
    NetLobbyState a;
    a.phase = NetPhase::ChoosingVehicles;
    a.settings.level = "SNOW3";
    a.settings.laps = 5;
    a.settings.ghosts = true;
    a.players.push_back(NetPlayer{0, NetLook{"HOTE", 3, 7, 30}, true, 2});
    a.players.push_back(NetPlayer{4, NetLook{"INVITE", 1, 12, 100}, false, -1});
    a.settings.races = 3;
    a.settings.random_circuit = true;
    a.series_done = 2;
    a.players[0].points = 18;
    a.players[1].points = 16;
    NetWriter w;
    write_lobby(w, a);
    NetLobbyState b;
    NetReader r(w.bytes().data(), w.bytes().size());
    const bool ok = read_lobby(r, b) && r.at_end();
    check(ok && b.phase == a.phase && b.settings.level == "SNOW3" && b.settings.laps == 5 &&
              b.settings.ghosts && b.players.size() == 2 && b.players[1].id == 4 &&
              b.players[1].look.name == "INVITE" && b.players[1].look.damage == 100 &&
              b.players[0].vehicle == 2 && b.players[1].vehicle == -1,
          "salon ecrit puis relu a l'identique");
    check(b.settings.races == 3 && b.settings.random_circuit && b.series_done == 2 &&
              b.players[0].points == 18 && b.players[1].points == 16,
          "la serie aussi : longueur, tirage, courses faites, points");
    check(net_series_points(1) == 10 && net_series_points(2) == 8 && net_series_points(5) == 2 &&
              net_series_points(6) == 0 && net_series_points(9) == 0,
          "bareme de l'executable : 10, 8, 6, 4, 2, 0");
    NetLobbyState c;
    NetReader shortr(w.bytes().data(), w.bytes().size() - 3);
    check(!read_lobby(shortr, c), "un paquet tronque est refuse");
    NetRaceStart s;
    s.race_id = 7;
    s.settings.level = "DIRT1";
    s.racers.push_back(NetRacer{0, 2, "JEEP", NetLook{"A", 0, 0, 30}});
    NetWriter ws;
    write_start(ws, s);
    NetRaceStart s2;
    NetReader rs(ws.bytes().data(), ws.bytes().size());
    check(read_start(rs, s2) && s2.race_id == 7 && s2.racers.size() == 1 &&
              s2.racers[0].vehicle == "JEEP" && s2.racers[0].grid == 2,
          "depart ecrit puis relu a l'identique");

    // The player's own logo travels as PNG bytes in the look; too many bytes, and it does not.
    NetLook own{"B", 0, 13, 30};
    own.custom_logo.assign(1500, 0x5a);
    NetLook huge = own;
    huge.custom_logo.assign(kNetCustomLogoMaxBytes + 1, 0x5a);
    NetWriter wl;
    write_look(wl, own);
    write_look(wl, huge);
    wl.u8(0x77);
    NetReader rl(wl.bytes().data(), wl.bytes().size());
    const NetLook own2 = read_look(rl);
    const NetLook huge2 = read_look(rl);
    check(rl.ok() && own2.logo == 13 && own2.custom_logo == own.custom_logo &&
              huge2.custom_logo.empty() && rl.u8() == 0x77 && rl.at_end(),
          "le logo du joueur voyage, un logo trop gros non");
}

int run_all(const std::string& game_dir, const std::string& level_name) {
    test_wire();

    LevelScene scene;
    if (!scene.load(game_dir, level_name)) {
        std::printf("level %s load failed\n", level_name.c_str());
        return 1;
    }
    RaceLevel level;
    build_race_level(scene, level_name, level, game_dir);
    // What a player may pick: the level's own list without the presentation helicopter, the
    // same rule as the menus (game/level_catalog.h).
    std::vector<std::string> choosable;
    for (const std::string& c : level.cars)
        if (!is_presentation_vehicle(c)) choosable.push_back(c);
    std::printf("\n%s : %zu vehicule(s) au choix, %d tour(s)\n", level_name.c_str(),
                choosable.size(), level.laps);

    Machine host, a, b;
    host.net = std::make_unique<NetGame>();
    a.net = std::make_unique<NetGame>();
    b.net = std::make_unique<NetGame>();
    host.name = "hote";
    a.name = "client A";
    b.name = "client B";
    std::vector<Machine*> all{&host, &a, &b};

    std::printf("\n2. LA DECOUVERTE\n");
    NetSettings settings;
    settings.level = level_name;
    settings.max_players = 3;
    check(host.net->host(NetLook{"HOTE", 0, 0, 30}, settings, true), "l'hote ouvre sa partie");
    host.net->set_settings(settings, choosable);
    DiscoveryBrowser browser;
    check(browser.open(true), "le navigateur ouvre sa socket");
    bool found = pump_until(all, [&] {
        browser.service(clock_ms());
        return !browser.games().empty();
    }, 4000);
    check(found, "la partie est trouvee");
    std::string addr = "127.0.0.1";
    uint16_t port = host.net->port();
    if (found) {
        const GameAnnounce& g = browser.games().front();
        std::printf("      %s:%u  \"%s\"  %s  %u/%u\n", g.address.c_str(), g.game_port,
                    g.host_name.c_str(), g.level.c_str(), g.players, g.max_players);
        check(g.game_port == port && g.level == level_name && g.players == 1 && g.max_players == 3,
              "l'annonce dit le bon port, le bon circuit, 1 joueur sur 3");
        addr = g.address;
    }

    std::printf("\n3. LE SALON\n");
    check(a.net->join(addr, port, NetLook{"ALICE", 2, 5, 60}), "A se connecte");
    check(b.net->join(addr, port, NetLook{"BOB", 4, 9, 0}), "B se connecte");
    check(pump_until(all, [&] {
              return a.net->joined() && b.net->joined() && host.net->lobby().players.size() == 3 &&
                     a.net->lobby().players.size() == 3 && b.net->lobby().players.size() == 3;
          }),
          "trois joueurs, vus pareil par les trois");
    Machine extra;
    extra.net = std::make_unique<NetGame>();
    std::vector<Machine*> with_extra{&host, &a, &b, &extra};
    extra.net->join("127.0.0.1", port, NetLook{"TROP", 0, 0, 30});
    check(pump_until(with_extra, [&] { return !extra.net->error().empty(); }),
          "un quatrieme est refuse : partie pleine");
    std::printf("      raison : %s\n", extra.net->error().c_str());
    pump(all, 20);
    check(host.net->lobby().players.size() == 3, "le refuse n'apparait pas dans le salon");
    const NetPlayer* alice = host.net->player(a.net->local_id());
    check(alice && alice->look.name == "ALICE" && alice->look.damage == 60 && alice->look.logo == 5,
          "l'hote voit le pseudo, le logo et les degats d'ALICE");

    check(!host.net->begin_vehicle_choice(), "pas de choix des vehicules tant que tous ne sont pas prets");
    a.net->set_ready(true);
    b.net->set_ready(true);
    check(pump_until(all, [&] { return host.net->everyone_ready(); }), "A et B se declarent prets");

    std::printf("\n4. LES VEHICULES ET LE DEPART\n");
    check(host.net->begin_vehicle_choice(), "l'hote lance le choix des vehicules");
    check(pump_until(all, [&] {
              return a.net->lobby().phase == NetPhase::ChoosingVehicles &&
                     b.net->lobby().phase == NetPhase::ChoosingVehicles;
          }),
          "les clients passent au choix des vehicules");
    const int nc = static_cast<int>(choosable.size());
    host.net->set_vehicle(0);
    a.net->set_vehicle(static_cast<int16_t>(1 % nc));
    b.net->set_vehicle(static_cast<int16_t>(2 % nc));
    check(pump_until(all, [&] {
              return host.net->pending_start() && a.net->pending_start() && b.net->pending_start();
          }),
          "tout le monde a choisi : le depart arrive chez les trois");
    if (!host.net->pending_start() || !a.net->pending_start() || !b.net->pending_start()) return 1;
    {
        const NetRaceStart& h = host.net->race();
        bool same = true;
        for (Machine* m : {&a, &b}) {
            const NetRaceStart& o = m->net->race();
            same = same && o.race_id == h.race_id && o.racers.size() == h.racers.size() &&
                   o.settings.level == h.settings.level;
            for (size_t i = 0; same && i < h.racers.size(); ++i)
                same = o.racers[i].vehicle == h.racers[i].vehicle &&
                       o.racers[i].grid == h.racers[i].grid &&
                       o.racers[i].look.damage == h.racers[i].look.damage;
        }
        check(same, "les trois ont recu le meme depart");
        for (const NetRacer& r : h.racers)
            std::printf("      coureur joueur %u %-8s place %u  %s\n", r.player_id,
                        r.look.name.c_str(), r.grid + 1, r.vehicle.c_str());
        check(a.net->local_racer() >= 0 && b.net->local_racer() >= 0 &&
                  a.net->local_racer() != b.net->local_racer(),
              "chacun connait son coureur");
    }
    for (Machine* m : all)
        if (!build_machine(*m, game_dir, scene, level)) {
            std::printf("  build failed on %s\n", m->name.c_str());
            return 1;
        }
    check(pump_until(all, [&] { return host.net->race_go() && a.net->race_go() && b.net->race_go(); }),
          "tous charges : le GO arrive chez les trois");

    // ⚠️ EVERYTHING FROM HERE RUNS WITH 20 % OF THE INPUT PACKETS LOST, on every machine: the
    // inputs are unreliable and repeated until acknowledged, so the race must still be the
    // reference at every tick - and the resync must still hold with its announcement and the
    // inputs on different channels.
    for (Machine* m : all) m->net->set_input_loss(20);
    std::printf("\n5. LA COURSE (20 %% des paquets d'entrees perdus)\n");
    const uint32_t kTicks = 1800;   // 50 s, the countdown included
    const std::vector<uint32_t> ref = reference_run(game_dir, scene, level, host.net->race(), kTicks);
    uint32_t frame = 0;
    int stalls = 0;
    const uint32_t t0 = clock_ms();
    while (clock_ms() - t0 < 60000) {
        ++frame;
        // A different servicing order every frame, and client B only gets one frame in three:
        // the slow machine everyone else has to wait for.
        const int order = static_cast<int>(frame % 3);
        for (int k = 0; k < 3; ++k) {
            Machine* m = all[static_cast<size_t>((k + order) % 3)];
            if (m == &b && frame % 3 != 0) continue;
            m->net->service(clock_ms());
            if (step_machine(*m, kTicks, 4) == 0 && m->session->ticks() < kTicks) ++stalls;
        }
        if (host.session->ticks() >= kTicks && a.session->ticks() >= kTicks &&
            b.session->ticks() >= kTicks)
            break;
    }
    pump(all, 30);
    auto first_diff = [&](const std::vector<uint32_t>& x) -> int {
        for (size_t i = 0; i < x.size() && i < ref.size(); ++i)
            if (x[i] != ref[i]) return static_cast<int>(i) + 1;
        return x.size() < ref.size() ? static_cast<int>(x.size()) + 1 : 0;
    };
    std::printf("      %u images, %d attentes, hote %u / A %u / B %u ticks\n", frame, stalls,
                host.session->ticks(), a.session->ticks(), b.session->ticks());
    check(first_diff(host.hashes) == 0, "l'hote = la reference, a chaque tick");
    check(first_diff(a.hashes) == 0, "A = la reference, a chaque tick");
    check(first_diff(b.hashes) == 0, "B = la reference, a chaque tick");
    check(host.net->desync_tick() == 0, "l'hote n'a vu aucune desynchronisation");
    std::printf("      empreintes comparees par l'hote : %u, entrees recues par l'hote : %u\n",
                host.net->stats().checksums_compared, host.net->stats().inputs_received);
    check(host.net->stats().checksums_compared >= 2 * (kTicks / kNetChecksumEvery) - 4,
          "l'hote a bien compare les empreintes des deux clients");
    {
        int moved = 0;
        for (int i = 0; i < host.session->racer_count(); ++i) {
            const VehicleSim& c = host.session->car(i);
            const RespawnPoint& st = host.session->start(i);
            const int64_t dx = c.world_x() - st.x, dz = c.world_z() - st.z;
            if (dx * dx + dz * dz > 2000LL * 2000) ++moved;
        }
        check(moved == host.session->racer_count(), "toutes les voitures ont roule (plus de 2000 u)");
    }

    std::printf("\n6. UNE DESYNCHRONISATION EST VUE, PUIS REPAREE\n");
    const uint32_t kMore = kTicks + 400;
    a.session->car(0).px += 512;   // one world unit, on A only
    const uint32_t nudged_at = a.session->ticks();
    // The desync is transient now - the resync clears it - so what each machine SAW is kept.
    uint32_t seen_host = 0, seen_a = 0, seen_b = 0, resync_at = 0;
    const uint32_t t1 = clock_ms();
    while (clock_ms() - t1 < 30000) {
        for (Machine* m : all) {
            m->net->service(clock_ms());
            step_machine(*m, kMore, 4);
        }
        if (!seen_host) seen_host = host.net->desync_tick();
        if (!seen_a) seen_a = a.net->desync_tick();
        if (!seen_b) seen_b = b.net->desync_tick();
        if (!resync_at) resync_at = host.net->resync_tick();
        if (host.session->ticks() >= kMore && a.session->ticks() >= kMore &&
            b.session->ticks() >= kMore)
            break;
    }
    pump(all, 30);
    std::printf("      pousse au tick %u, desynchro annoncee au tick %u, resynchro au tick %u\n",
                nudged_at, seen_host, resync_at);
    check(seen_host > nudged_at && seen_host <= nudged_at + kNetChecksumEvery,
          "l'hote la voit a la premiere empreinte qui suit");
    check(seen_a == seen_host && seen_b == seen_host, "et la dit aux deux clients");
    check(resync_at > seen_host, "l'hote fixe un tick de resynchronisation apres elle");
    check(host.net->resyncs() == 1 && a.resyncs_loaded == 1 && b.resyncs_loaded == 1,
          "l'instantane de l'hote est envoye une fois, et charge par les deux clients");
    check(host.session->checksum() == a.session->checksum() &&
              host.session->checksum() == b.session->checksum() &&
              host.session->ticks() == a.session->ticks() && host.session->ticks() == b.session->ticks(),
          "les trois machines sont de nouveau identiques");
    check(host.net->desync_tick() == 0 && a.net->desync_tick() == 0 && b.net->desync_tick() == 0,
          "et plus personne n'affiche la desynchronisation");
    {
        // Identical since the resync, at every tick - not only at the end.
        int diverged = 0;
        const size_t n = std::min(host.hashes.size(), std::min(a.hashes.size(), b.hashes.size()));
        for (size_t i = resync_at; i < n; ++i)
            if (host.hashes[i] != a.hashes[i] || host.hashes[i] != b.hashes[i]) ++diverged;
        check(diverged == 0, "a chaque tick depuis la resynchronisation");
    }

    // The series: what the host's race screen does once per race (RaceScreen::push_results).
    host.net->record_race({{host.net->local_id(), 2}, {a.net->local_id(), 1}, {b.net->local_id(), 7}});
    const bool series_seen = pump_until(all, [&] {
        const NetPlayer* pa = b.net->player(a.net->local_id());
        return pa && pa->points == 10 && b.net->lobby().series_done == 1;
    });
    const NetPlayer* ph = a.net->player(host.net->local_id());
    const NetPlayer* pb = a.net->player(b.net->local_id());
    check(series_seen && ph && ph->points == 8 && pb && pb->points == 0,
          "serie : les points de l'hote arrivent chez les clients (10 / 8 / 0 pour une 7e place)");

    std::printf("\n7. UN JOUEUR S'EN VA\n");
    // A fresh race: back to the lobby, everyone ready, same vehicles.
    host.net->back_to_lobby();
    pump_until(all, [&] {
        return a.net->lobby().phase == NetPhase::Lobby && b.net->lobby().phase == NetPhase::Lobby;
    });
    a.net->set_ready(true);
    b.net->set_ready(true);
    pump_until(all, [&] { return host.net->everyone_ready(); });
    host.net->begin_vehicle_choice();
    pump_until(all, [&] { return a.net->lobby().phase == NetPhase::ChoosingVehicles &&
                                 b.net->lobby().phase == NetPhase::ChoosingVehicles; });
    host.net->set_vehicle(0);
    a.net->set_vehicle(0);
    b.net->set_vehicle(0);
    check(pump_until(all, [&] {
              return host.net->pending_start() && a.net->pending_start() && b.net->pending_start();
          }),
          "une deuxieme course demarre");
    check(host.net->race().race_id == 1 && host.net->race().racers[0].grid == 1,
          "les places ont tourne d'un cran (mode 0x23 de l'original)");
    for (Machine* m : all) build_machine(*m, game_dir, scene, level);
    pump_until(all, [&] { return host.net->race_go() && a.net->race_go() && b.net->race_go(); });
    const uint32_t kLeaveAt = 400, kEnd = 1200;
    const int b_racer = b.net->local_racer();
    std::vector<Machine*> left{&host, &a};
    const uint32_t t2 = clock_ms();
    bool b_gone = false;
    while (clock_ms() - t2 < 60000) {
        for (Machine* m : all) {
            if (m == &b && b_gone) continue;
            m->net->service(clock_ms());
            step_machine(*m, m == &b ? kLeaveAt : kEnd, 4);
        }
        if (!b_gone && b.session->ticks() >= kLeaveAt) {
            b.net->leave();
            b_gone = true;
        }
        if (host.session->ticks() >= kEnd && a.session->ticks() >= kEnd) break;
    }
    std::printf("      B part au tick %u ; hote %u, A %u ticks\n", kLeaveAt, host.session->ticks(),
                a.session->ticks());
    check(host.session->ticks() >= kEnd && a.session->ticks() >= kEnd,
          "l'hote et A continuent sans B");
    bool same = host.hashes.size() >= kEnd && a.hashes.size() >= kEnd;
    for (size_t i = 0; same && i < kEnd; ++i) same = host.hashes[i] == a.hashes[i];
    check(same, "l'hote et A restent d'accord, tick par tick");
    check(!host.net->racer_present(b_racer) && host.net->racer_present(a.net->local_racer()),
          "l'hote sait B parti, A present");
    check(host.net->lobby().players.size() == 2, "B a quitte le salon de l'hote");

    a.net->leave();
    host.net->leave();

    std::printf("\n8. HUIT JOUEURS : LA GRILLE PROLONGEE\n");
    {
        constexpr int kPlayers = 8;
        std::vector<std::unique_ptr<Machine>> big;
        std::vector<Machine*> ms;
        for (int i = 0; i < kPlayers; ++i) {
            big.push_back(std::make_unique<Machine>());
            big.back()->net = std::make_unique<NetGame>();
            big.back()->name = "joueur " + std::to_string(i);
            ms.push_back(big.back().get());
        }
        NetSettings s8;
        s8.level = level_name;
        s8.max_players = kPlayers;
        check(ms[0]->net->host(NetLook{"HOTE", 0, 0, 30}, s8, true), "un hote a 8 places");
        ms[0]->net->set_settings(s8, choosable);
        const uint16_t p8 = ms[0]->net->port();
        for (int i = 1; i < kPlayers; ++i)
            ms[static_cast<size_t>(i)]->net->join("127.0.0.1", p8,
                                                  NetLook{"J" + std::to_string(i), 0, 0, 30});
        check(pump_until(ms, [&] { return ms[0]->net->lobby().players.size() == kPlayers; }),
              "sept clients entrent");
        for (int i = 1; i < kPlayers; ++i) ms[static_cast<size_t>(i)]->net->set_ready(true);
        pump_until(ms, [&] { return ms[0]->net->everyone_ready(); });
        ms[0]->net->begin_vehicle_choice();
        pump_until(ms, [&] {
            for (Machine* m : ms)
                if (m->net->lobby().phase != NetPhase::ChoosingVehicles) return false;
            return true;
        });
        for (int i = 0; i < kPlayers; ++i)
            ms[static_cast<size_t>(i)]->net->set_vehicle(
                static_cast<int16_t>(i % static_cast<int>(choosable.size())));
        check(pump_until(ms, [&] {
                  for (Machine* m : ms)
                      if (!m->net->pending_start()) return false;
                  return true;
              }),
              "le depart arrive chez les huit");
        int max_grid = 0;
        for (const NetRacer& r : ms[0]->net->race().racers) max_grid = std::max<int>(max_grid, r.grid);
        check(max_grid == kPlayers - 1, "les places vont jusqu'a la huitieme");
        bool built = true;
        for (Machine* m : ms) built = built && build_machine(*m, game_dir, scene, level);
        check(built, "les huit construisent la course");
        pump_until(ms, [&] {
            for (Machine* m : ms)
                if (!m->net->race_go()) return false;
            return true;
        });
        const uint32_t k8 = 700;
        const std::vector<uint32_t> ref8 = reference_run(game_dir, scene, level, ms[0]->net->race(), k8);
        const uint32_t t8 = clock_ms();
        while (clock_ms() - t8 < 60000) {
            bool done = true;
            for (Machine* m : ms) {
                m->net->service(clock_ms());
                step_machine(*m, k8, 4);
                done = done && m->session->ticks() >= k8;
            }
            if (done) break;
        }
        bool agree = true;
        for (Machine* m : ms)
            for (uint32_t i = 0; agree && i < k8; ++i)
                agree = i < m->hashes.size() && m->hashes[i] == ref8[i];
        check(agree, "les huit = la reference, a chaque tick");
        {
            const RaceSession& rs = *ms[0]->session;
            int inside = 0;
            for (int i = 0; i < rs.racer_count(); ++i)
                for (int j = i + 1; j < rs.racer_count(); ++j) {
                    const int64_t dx = rs.start(i).x - rs.start(j).x, dz = rs.start(i).z - rs.start(j).z;
                    if (dx * dx + dz * dz < 300LL * 300) ++inside;
                }
            check(inside == 0, "aucune place a moins de 300 unites d'une autre");
        }
        check(ms[0]->net->desync_tick() == 0, "aucune desynchronisation");
        for (Machine* m : ms) m->net->leave();
    }

    std::printf("\n%s : %d echec(s)\n", g_failures ? "ECHEC" : "TOUT EST BON", g_failures);
    return g_failures ? 1 : 0;
}

// --- one machine of a multi-process test --------------------------------------------------------

int run_process(const std::string& game_dir, const std::string& level_name, bool is_host,
                int players, uint32_t ticks) {
    LevelScene scene;
    if (!scene.load(game_dir, level_name)) return 1;
    RaceLevel level;
    build_race_level(scene, level_name, level, game_dir);
    Machine m;
    m.net = std::make_unique<NetGame>();
    std::vector<Machine*> me{&m};
    const uint32_t t0 = clock_ms();
    if (is_host) {
        NetSettings s;
        s.level = level_name;
        if (!m.net->host(NetLook{"HOTE", 0, 0, 30}, s, true)) return 1;
        m.net->set_settings(s, level.cars);
        pump_until(me, [&] {
            return static_cast<int>(m.net->lobby().players.size()) >= players && m.net->everyone_ready();
        }, 30000);
        m.net->begin_vehicle_choice();
        m.net->set_vehicle(0);
    } else {
        DiscoveryBrowser browser;
        browser.open(true);
        if (!pump_until(me, [&] {
                browser.service(clock_ms());
                return !browser.games().empty();
            }, 15000)) {
            std::printf("no game found\n");
            return 1;
        }
        const GameAnnounce g = browser.games().front();
        m.net->join(g.address, g.game_port, NetLook{"CLIENT", 1, 1, 30});
        pump_until(me, [&] { return m.net->joined() || !m.net->error().empty(); }, 10000);
        m.net->set_ready(true);
        pump_until(me, [&] { return m.net->lobby().phase == NetPhase::ChoosingVehicles; }, 30000);
        m.net->set_vehicle(static_cast<int16_t>((m.net->local_id()) % std::max<size_t>(1, level.cars.size())));
    }
    if (!pump_until(me, [&] { return m.net->pending_start() != nullptr; }, 30000)) {
        std::printf("no start\n");
        return 1;
    }
    build_machine(m, game_dir, scene, level);
    pump_until(me, [&] { return m.net->race_go(); }, 30000);
    while (m.session->ticks() < ticks && m.net->error().empty() && clock_ms() - t0 < 120000) {
        m.net->service(clock_ms());
        if (step_machine(m, ticks, 8) == 0) SDL_Delay(1);
    }
    // Stay a moment so the last inputs and checksums reach everyone.
    pump(me, 300);
    std::printf("racer %d ticks %u", m.net->local_racer(), m.session->ticks());
    for (uint32_t t = 100; t <= m.hashes.size(); t += 100) std::printf(" %u:%08x", t, m.hashes[t - 1]);
    std::printf("\ndesync %u compared %u\n", m.net->desync_tick(), m.net->stats().checksums_compared);
    m.net->leave();
    return 0;
}

// --- THE AUTODRIVE RACE, AND WHY A CAR GOES OVER --------------------------------------------
//
// `net_bench - LEVEL flips [PLAYERS] [TICKS] [VEHICLE]`: the race `openbigred -NETHOST=N
// -NETJOIN` runs with its automatic lobby - every player on the level's first vehicle, damage
// 30, grid blocks in join order - with the game's AUTODRIVE (RaceScreen::update: a pure function
// of the racer and the tick). No network: every machine of that race computes this. The user
// saw one of the two cars go over again and again (2026-09-27): each time a car goes past 90
// degrees of roll or folds its pitch, the ticks before it are printed.
//
// ⚠️ The game samples the autodrive once per FRAME and sends it for `ticks + 1 + delay`; a frame
// that runs two ticks sends the same sample twice. Here the input of tick T is the sample of
// T - delay - 1, so a change of burst (every 20 ticks) can land a tick apart from the game's.
uint8_t autodrive(int session_racer, uint32_t ticks) {
    uint32_t s = 2654435761u * static_cast<uint32_t>(session_racer + 1) + (ticks / 20) * 40503u;
    s ^= s >> 13;
    s *= 2246822519u;
    s ^= s >> 16;
    VehicleInput in;
    in.accel = true;
    in.left = (s % 3) == 0;
    in.right = (s % 3) == 1;
    in.nitro = (s % 17) == 0;
    return pack_input(in);
}

int run_flips(const std::string& game_dir, const std::string& level_name, int players,
              uint32_t ticks, const std::string& vehicle_arg) {
    LevelScene scene;
    if (!scene.load(game_dir, level_name)) return 1;
    RaceLevel level;
    build_race_level(scene, level_name, level, game_dir);
    RaceRules rules = RaceRules::network();
    if (const char* v = std::getenv("BENCH_AIRROLL")) rules.air_roll_cap_value = std::atoi(v);
    if (const char* v = std::getenv("BENCH_AIRDAMP")) rules.air_damp_shift = std::atoi(v);
    if (const char* v = std::getenv("BENCH_AIRDAMP_ROLL")) rules.air_roll_damp_shift = std::atoi(v);
    std::vector<std::string> choosable;
    for (const std::string& c : level.cars)
        if (!is_presentation_vehicle(c)) choosable.push_back(c);
    if (choosable.empty()) return 1;
    const std::string vehicle = vehicle_arg.empty() ? choosable[0] : vehicle_arg;
    std::vector<FieldHuman> humans;
    for (int i = 0; i < players; ++i)
        humans.push_back(FieldHuman{static_cast<uint8_t>(i), vehicle, 30});
    RaceField field;
    if (!build_race_field(game_dir, scene, level, humans, false, field)) return 1;
    SceneryState world;
    world.resize(scene.instances.size());
    RaceSession session;
    session.rules = rules;
    session.configure(level.terrain, &world, level.gates, level.laps, level.countdown_ticks,
                      level.lines, level.grid, level.nitros, field.racers, level.origin,
                      level.rates);
    std::printf("\nFLIPS  %s, %d x %s, autodrive, %u ticks\n", level_name.c_str(), players,
                vehicle.c_str(), ticks);
    const int delay = kNetInputDelay;
    std::vector<VehicleInput> in(static_cast<size_t>(session.racer_count()));
    // The last 60 ticks of every human, to print when it goes over.
    struct Row {
        uint32_t t;
        int32_t pitch, roll, yaw, pitch_rate, roll_rate, yaw_rate, speed, vy, tpitch, troll;
        uint16_t contact, near;
        uint8_t in;
    };
    std::vector<std::vector<Row>> hist(static_cast<size_t>(players));
    std::vector<bool> over(static_cast<size_t>(players), false);
    std::vector<int> count(static_cast<size_t>(players), 0);
    for (uint32_t t = 1; t <= ticks; ++t) {
        for (int h = 0; h < players; ++h) {
            const int s = field.session_of[static_cast<size_t>(h)];
            in[static_cast<size_t>(s)] =
                t <= static_cast<uint32_t>(delay)
                    ? VehicleInput()
                    : unpack_input(autodrive(s, t - static_cast<uint32_t>(delay) - 1));
        }
        session.tick(in.data());
        if (t % 250 == 0) std::printf("  tick %u checksum %08x\n", t, session.checksum());
        for (int h = 0; h < players; ++h) {
            const int s = field.session_of[static_cast<size_t>(h)];
            const VehicleSim& c = session.car(s);
            auto& hh = hist[static_cast<size_t>(h)];
            hh.push_back({t, c.pitch_bam16(), c.roll_bam16(), c.yaw_bam16(), c.pitch_rate,
                          c.roll_rate, c.yaw_rate, c.speed, c.vy, c.terrain_pitch, c.terrain_roll,
                          c.contact, c.near_ground,
                          pack_input(in[static_cast<size_t>(s)])});
            if (hh.size() > 70) hh.erase(hh.begin());
            const bool now_over = c.rolled_over || c.upside_down;
            if (now_over && !over[static_cast<size_t>(h)]) {
                ++count[static_cast<size_t>(h)];
                std::printf("\n  racer %d (session %d) GOES OVER at tick %u  x %d z %d  "
                            "(rolled_over %d, upside_down %d)\n",
                            h, s, t, c.world_x(), c.world_z(), c.rolled_over, c.upside_down);
                std::printf("     tick   pitch    roll  p_rate  r_rate  y_rate  speed     vy  "
                            "t_pitch t_roll contact near  in\n");
                for (const Row& r : hh)
                    std::printf("    %5u %7.1f %7.1f %7d %7d %7d %6d %6d %7.1f %6.1f   %02x     %02x  %c%c%c%c\n",
                                r.t, r.pitch * 360.0 / 65536.0, r.roll * 360.0 / 65536.0,
                                r.pitch_rate, r.roll_rate, r.yaw_rate, r.speed, r.vy,
                                static_cast<int16_t>(r.tpitch) * 360.0 / 65536.0,
                                static_cast<int16_t>(r.troll) * 360.0 / 65536.0, r.contact,
                                r.near, (r.in & 1) ? 'A' : '.', (r.in & 4) ? 'L' : '.',
                                (r.in & 8) ? 'R' : '.', (r.in & 16) ? 'N' : '.');
            }
            over[static_cast<size_t>(h)] = now_over;
        }
    }
    for (int h = 0; h < players; ++h) std::printf("  racer %d went over %d time(s)\n", h, count[static_cast<size_t>(h)]);
    return 0;
}

// --- the LATENCY MODEL -------------------------------------------------------------------------
//
// `net_bench - lag`: how the lockstep race behaves over a SLOW network, with no network at all.
// Virtual machines at 60 frames a second, each with the game's real LockstepInputs and the
// game's real pacing rule (lockstep_gate), joined in a star through the host exactly like
// NetGame: a client sends to the host, the host relays to everyone else when it next services
// (once per frame). Every link is ENet's RELIABLE, IN-ORDER channel: a lost packet is sent
// again after a round-trip timeout, and everything behind it waits for it.
// Virtual time, no sleeping: a minute and a half of race costs milliseconds, so it can sweep.
// No physics here - WHEN a tick runs does not depend on what it computes.

struct LagPacket {
    double at;
    int racer;
    uint32_t tick;
};

struct LagLink {
    std::vector<LagPacket> q;   // in delivery order
    double last = 0.0;          // delivery time of the last packet queued (in-order channel)
};

struct LagMachine {
    LockstepInputs lk;
    double phase = 0.0;         // time of the first frame
    double go_at = -1.0;
    double acc = 0.0;           // time owed to the simulation, in ticks
    uint32_t ticks = 0;
    uint32_t sent_upto = 0;
    double stall = 0.0;         // ms waiting with no tick run, as the banner counts it
    double last_tick_at = -1.0;
    int banners = 0;            // stalls of 300 ms or more: "WAITING FOR ..." on screen
    double worst_freeze = 0.0;  // longest time with no tick run, in ms
    int hitches = 0;            // freezes longer than 3 ticks: a visible jerk
};

struct LagSetup {
    int players = 2;
    int delay = kNetInputDelay;
    double one_way = 0.0;       // ms, every host-client link
    double jitter = 0.0;        // ms, the MEAN of an exponential extra delay per packet
    double loss = 0.0;          // probability that one transmission is lost
    // NOT what the game does today: inputs sent UNRELIABLE, each packet repeating the last few.
    // A lost packet costs one tick (the next one carries the same input), nothing waits for it.
    bool redundant = false;
    double seconds = 90.0;
    uint32_t seed = 1;
};

struct LagResult {
    double speed = 1.0;         // race time / real time, slowest machine
    int banners = 0;
    double worst_freeze = 0.0;
    int hitches = 0;
};

LagResult run_lag(const LagSetup& cfg) {
    constexpr double kFrame = 1000.0 / 60.0;
    constexpr double kTick = static_cast<double>(kPhysicsTickMs);
    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const int n = cfg.players;
    std::vector<LagMachine> m(static_cast<size_t>(n));
    for (LagMachine& x : m) {
        x.lk.configure(n, cfg.delay);
        x.phase = uni(rng) * kFrame;
    }
    std::vector<LagLink> links(static_cast<size_t>(n * n));   // [from * n + to]
    const double rto = 2.0 * cfg.one_way + 4.0 * cfg.jitter + 2.0 * kFrame;   // ENet: rtt + 4 var
    auto flight = [&]() {
        double d = cfg.one_way;
        if (cfg.jitter > 0.0) d += -cfg.jitter * std::log(1.0 - uni(rng));
        return d;
    };
    auto send = [&](int from, int to, double now, int racer, uint32_t tick) {
        LagLink& l = links[static_cast<size_t>(from * n + to)];
        const double ph = m[static_cast<size_t>(from)].phase;
        double t = now, backoff = rto;
        if (cfg.redundant) {
            // The same input rides in the next packets, one per tick; no ordering to wait on.
            while (cfg.loss > 0.0 && uni(rng) < cfg.loss) t += kTick;
            l.q.push_back({t + flight(), racer, tick});
            std::sort(l.q.begin(), l.q.end(),
                      [](const LagPacket& a, const LagPacket& b) { return a.at < b.at; });
            return;
        }
        while (cfg.loss > 0.0 && uni(rng) < cfg.loss) {
            // Resent at the sender's first service after the timeout; ENet doubles it each time.
            t = ph + std::ceil((t + backoff - ph) / kFrame) * kFrame;
            backoff *= 2.0;
        }
        const double at = std::max(t + flight(), l.last);
        l.last = at;
        l.q.push_back({at, racer, tick});
    };
    m[0].go_at = 0.0;   // the host says GO and starts at once; the clients when it reaches them
    for (int c = 1; c < n; ++c) m[static_cast<size_t>(c)].go_at = flight();

    const double end = cfg.seconds * 1000.0;
    std::vector<double> next_frame(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) next_frame[static_cast<size_t>(i)] = m[static_cast<size_t>(i)].phase;
    for (;;) {
        int i = 0;
        for (int k = 1; k < n; ++k)
            if (next_frame[static_cast<size_t>(k)] < next_frame[static_cast<size_t>(i)]) i = k;
        const double now = next_frame[static_cast<size_t>(i)];
        if (now > end) break;
        next_frame[static_cast<size_t>(i)] += kFrame;
        LagMachine& me = m[static_cast<size_t>(i)];
        // 1. NetGame::service: everything that has arrived; the host relays what it receives.
        for (int from = 0; from < n; ++from) {
            LagLink& l = links[static_cast<size_t>(from * n + i)];
            size_t k = 0;
            for (; k < l.q.size() && l.q[k].at <= now; ++k) {
                me.lk.submit(l.q[k].racer, l.q[k].tick, 0);
                if (i == 0)
                    for (int to = 1; to < n; ++to)
                        if (to != from) send(0, to, now, l.q[k].racer, l.q[k].tick);
            }
            l.q.erase(l.q.begin(), l.q.begin() + static_cast<std::ptrdiff_t>(k));
        }
        if (me.go_at < 0.0 || now < me.go_at) continue;
        if (me.last_tick_at < 0.0) me.last_tick_at = now;
        // 2. RaceScreen::step_network, the pacing only.
        me.acc += kFrame / kTick;
        bool waiting = false;
        int ran = 0;
        while (me.acc >= 1.0 && ran < kLockstepMaxTicksPerFrame) {
            const uint32_t next = me.ticks + 1;
            const uint32_t due = me.lk.schedule(next);
            if (me.sent_upto < due) {
                me.sent_upto = due;
                me.lk.submit(i, due, 0);
                if (i == 0) {
                    for (int to = 1; to < n; ++to) send(0, to, now, 0, due);
                } else {
                    send(i, 0, now, i, due);
                }
            }
            const LockstepGate gate = lockstep_gate(me.lk, next, me.acc);
            if (gate == LockstepGate::Wait) {
                waiting = true;
                break;
            }
            if (gate == LockstepGate::Hold) break;
            me.acc -= 1.0;
            me.lk.release_before(next);
            me.ticks = next;
            ++ran;
        }
        if (waiting && me.acc > kLockstepWaitBankTicks) me.acc = kLockstepWaitBankTicks;
        // 3. What the player sees.
        if (ran > 0) {
            const double gap = now - me.last_tick_at;
            if (gap > me.worst_freeze) me.worst_freeze = gap;
            if (gap > 3.0 * kTick + kFrame) ++me.hitches;
            me.last_tick_at = now;
            if (me.stall >= 300.0) ++me.banners;
            me.stall = 0.0;
        } else if (waiting) {
            me.stall += kFrame;
        }
    }
    LagResult r;
    for (const LagMachine& x : m) {
        r.speed = std::min(r.speed, x.ticks * kTick / (end - x.go_at));
        r.banners = std::max(r.banners, x.banners);
        r.worst_freeze = std::max(r.worst_freeze, x.worst_freeze);
        r.hitches = std::max(r.hitches, x.hitches);
    }
    return r;
}

// The table: the PING (round trip, what a VPN's ping says) down the side, one column per kind
// of network, the worst of five draws in each cell.
int run_lag_sweep(int players, int delay, bool redundant) {
    std::printf("LATENCY MODEL - %d players, star through the host, input delay %d ticks (%d ms), "
                "90 s of race at 60 fps\n  inputs: %s\n", players, delay, delay * kPhysicsTickMs,
                redundant ? "REDUNDANT, unreliable (not the game today)" : "reliable, in order (the game today)");
    std::printf("  speed = race time / real time (slowest machine) | banners = stalls >= 300 ms\n"
                "  hitches = freezes > 3 ticks | freeze = longest time with no tick | worst of 5 draws\n\n");
    struct Net {
        const char* name;
        double jitter;
        double loss;
    };
    const Net nets[] = {{"clean", 0.0, 0.0},
                        {"jitter 10 ms", 10.0, 0.0},
                        {"jitter 10 + 1% loss", 10.0, 0.01},
                        {"jitter 20 + 2% loss", 20.0, 0.02}};
    std::printf("     ping |");
    for (const Net& nt : nets) std::printf(" %-27s|", nt.name);
    std::printf("\n          |");
    for (const Net& nt : nets) {
        (void)nt;
        std::printf(" speed bann hitch  freeze |");
    }
    std::printf("\n");
    const int pings[] = {0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 250, 300};
    for (int ping : pings) {
        std::printf("  %4d ms |", ping);
        for (const Net& nt : nets) {
            LagResult worst;
            for (uint32_t seed = 1; seed <= 5; ++seed) {
                LagSetup c;
                c.players = players;
                c.delay = delay;
                c.one_way = ping / 2.0;
                c.jitter = nt.jitter;
                c.loss = nt.loss;
                c.seed = seed;
                c.redundant = redundant;
                const LagResult r = run_lag(c);
                worst.speed = std::min(worst.speed, r.speed);
                worst.banners = std::max(worst.banners, r.banners);
                worst.worst_freeze = std::max(worst.worst_freeze, r.worst_freeze);
                worst.hitches = std::max(worst.hitches, r.hitches);
            }
            std::printf(" %4.0f%% %4d %5d %5.0f ms |", worst.speed * 100.0, worst.banners,
                        worst.hitches, worst.worst_freeze);
        }
        std::printf("\n");
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    SDL_Init(SDL_INIT_TIMER);
    const std::string game_dir = (argc > 1 && std::string(argv[1]) != "-")
                                     ? argv[1]
                                     : bigred::find_game_dir();
    const std::string level = (argc > 2) ? argv[2] : "DIRT1";
    int rc = 0;
    if (argc > 3 && std::string(argv[3]) == "flips")
        rc = run_flips(game_dir, level, argc > 4 ? std::atoi(argv[4]) : 2,
                       argc > 5 ? static_cast<uint32_t>(std::atoi(argv[5])) : 3000,
                       argc > 6 ? argv[6] : "");
    else if (argc > 2 && std::string(argv[2]) == "lag")
        rc = run_lag_sweep(argc > 3 ? std::atoi(argv[3]) : 2, argc > 4 ? std::atoi(argv[4]) : kNetInputDelay,
                           argc > 5 && std::string(argv[5]) == "redundant");
    else if (argc > 3 && std::string(argv[3]) == "host")
        rc = run_process(game_dir, level, true, argc > 4 ? std::atoi(argv[4]) : 2,
                         argc > 5 ? static_cast<uint32_t>(std::atoi(argv[5])) : 1500);
    else if (argc > 3 && std::string(argv[3]) == "join")
        rc = run_process(game_dir, level, false, 0,
                         argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 1500);
    else
        rc = run_all(game_dir, level);
    SDL_Quit();
    return rc;
}
