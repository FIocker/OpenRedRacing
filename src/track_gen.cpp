// track_gen - the circuit generator's test bench, with a window.
//
// Generates a circuit from a THEME (DIRT, CITY, SNOW, RIG, MOON, WATER, CHOPPER) and a SEED,
// and shows it as map_preview shows a shipped level: the same world renderer, a free camera,
// the gate ring and the CHECKP lines drawn over it. This is where the generator is tried and
// judged BEFORE it goes into the game (the method the
// user fixed): nothing here is reachable from openbigred.
//
//   track_gen [game_dir|-] [THEME] [SEED] [options]
//     -SHOT=f.bmp [-SHOTAT=S]  the window after S seconds (2 by default), then quit
//     -TOP=f.png               the circuit from above, one pixel per texel (no window needed)
//     -NOWINDOW                with -TOP: write it and quit, no window at all
//     -CAM=GRID                start on the grid, a driver's view (G in the window)
//     -DRIVE                   an autopilot drives one lap in each of the theme's vehicles
//     -CORRIDOR                the old method (the donor's landscape carried along the road,
//                              the massif) instead of the layers (gen/track_layers.h)
//     -DUMP=DIR                every generated file written into DIR (which must exist)
//     -REAL=LEVEL              a SHIPPED circuit instead, shown the same way - to compare
//     -REPAINT                 with -REAL: its ground tiles laid again by the autotiler from
//                              the vertex materials alone (gen/autotile.h), to judge by eye
//     -PAINT=M,Z,X,R           with -REAL: paint ground M (the index -REAL prints) in a disc of
//                              radius R vertices round vertex (X,Z), as an editor's brush would,
//                              then repair and lay the tiles (gen/autotile.h); repeatable
//     -PAINTLINE=M,Z0,X0,Z1,X1,W  the same along a stroke W vertices wide
//     --autotile=L1,L2 [-MIN=P] the autotiler's round trip on shipped levels, and quit;
//                              exit 1 if one comes back under P per mille identical
//     --analyse                what each theme's circuits teach the generator, and quit
//
// Keys: N / P next / previous seed, R a random seed, PageUp / PageDown the theme, T the view
// from above to track_top.png, C the ring on/off, arrows + right mouse + wheel: the camera.

#include <SDL.h>
#include <SDL_opengl.h>

#include "formats/game_files.h"
#include "formats/level_script.h"
#include "formats/terrain.h"
#include "gen/autotile.h"
#include "gen/track_generator.h"
#include "render/gl_loader.h"
#include "render/mat4.h"
#include "scene/gate_overlay.h"
#include "scene/level_scene.h"
#include "scene/race_level.h"
#include "scene/world_renderer.h"
#include "sim/fixed.h"
#include "sim/race.h"
#include "sim/vehicle.h"
#include "third_party/lodepng.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using bigred::kRenderScale;

namespace {

// The circuit from above, three pixels a texel, as a PNG.
bool write_top_png(const bigred::gen::Track& t, const std::string& path) {
    const std::vector<uint8_t> rgb = bigred::gen::top_view(t);
    const int scale = 3;   // 768 x 768: big enough to read the gates
    const int w = t.map_w * scale, h = t.map_h * scale;
    std::vector<unsigned char> out(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // X mirrored as the world is drawn, z down the image.
            const int mx = t.map_w - 1 - x / scale, mz = y / scale;
            std::memcpy(&out[(static_cast<size_t>(y) * w + x) * 3],
                        &rgb[(static_cast<size_t>(mz) * t.map_w + mx) * 3], 3);
        }
    return lodepng::encode(path, out, static_cast<unsigned>(w), static_cast<unsigned>(h), LCT_RGB) == 0;
}

// -DRIVE: can the circuit be driven? One car of each of the theme's vehicles, alone, driven
// round one lap by a small autopilot that aims at the centre line some way ahead - the first
// step towards the driving AI a generated circuit needs, since it has no recorded ghosts.
// It reports the lap time, how often it had to press ALT (stuck, or off the road), and how
// many ticks it spent against scenery.
void drive_test(const std::string& game_dir, const bigred::gen::Track& track, bigred::LevelScene& scene) {
    for (const std::string& vehicle : track.cars) {
        bigred::RaceLevel rl;
        bigred::build_race_level(scene, track.name, rl);
        bigred::RaceField field;
        bigred::FieldHuman me;
        me.vehicle = vehicle;
        if (!bigred::build_race_field(game_dir, scene, rl, {me}, false, field)) {
            std::printf("[drive] %s : vehicule introuvable\n", vehicle.c_str());
            continue;
        }
        bigred::SceneryState world;
        world.resize(scene.instances.size());
        bigred::RaceSession session;
        session.configure(rl.terrain, &world, rl.gates, 1, rl.countdown_ticks, rl.lines, rl.grid,
                          rl.nitros, field.racers, rl.origin);
        const std::vector<bigred::gen::Point>& c = track.centre;
        const int n = static_cast<int>(c.size());
        int idx = 0, best_idx = 0, stall = 0, recovers = 0, contact_ticks = 0, lap_tick = -1;
        int64_t travelled = 0;
        std::vector<int32_t> altitude;   // above the ground, world units, every tick once started
        const int kMaxTicks = 7000;
        for (int t = 0; t < kMaxTicks; ++t) {
            const bigred::VehicleSim& car = session.car(0);
            const int32_t x = car.world_x(), z = car.world_z();
            // The nearest centre-line sample, searched around the last one.
            int64_t best = INT64_MAX;
            int found = idx;
            for (int k = -30; k <= 60; ++k) {
                const int i = (idx + k + n) % n;
                const int64_t dx = c[static_cast<size_t>(i)].x - x, dz = c[static_cast<size_t>(i)].z - z;
                if (dx * dx + dz * dz < best) {
                    best = dx * dx + dz * dz;
                    found = i;
                }
            }
            const int step = (found - idx + n) % n;
            if (step < n / 2) travelled += step;
            idx = found;
            const int ahead = 10 + std::abs(car.speed) / 2500;
            const bigred::gen::Point& aim = c[static_cast<size_t>((idx + ahead) % n)];
            const int32_t want = bigred::fx::atan2_bam16(aim.x - x, aim.z - z);
            const int16_t err = static_cast<int16_t>(want - (car.yaw >> 8));
            bigred::VehicleInput in;
            in.accel = true;
            if (err > 0x300) in.right = true;
            else if (err < -0x300) in.left = true;
            if (std::abs(static_cast<int>(err)) > 0x2800 && car.speed > 15000) {
                in.accel = false;
                in.brake = true;
            }
            // Stuck: no progress along the line for three seconds.
            if (session.race().started()) {
                if (travelled > best_idx) {
                    best_idx = static_cast<int>(travelled);
                    stall = 0;
                } else if (++stall > 110) {
                    in.recover = true;
                    ++recovers;
                    stall = 0;
                }
            }
            session.tick(&in);
            if (session.car(0).scenery_hit) ++contact_ticks;
            if (session.race().started()) {
                const bigred::VehicleSim& cc = session.car(0);
                altitude.push_back((rl.terrain.height_fx(cc.px, cc.pz) - cc.py) >> 9);
            }
            if (session.race().progress(0).lap >= 1) {
                lap_tick = t;
                break;
            }
        }
        const double secs = lap_tick >= 0 ? lap_tick * 0.028 : 0.0;
        if (lap_tick >= 0)
            std::printf("[drive] %-8s tour boucle en %.1f s (%d ticks, depart compris), %d ALT, %d ticks contre le decor\n",
                        vehicle.c_str(), secs, lap_tick, recovers, contact_ticks);
        else {
            const bigred::RacerProgress& p = session.race().progress(0);
            std::sort(altitude.begin(), altitude.end());
            if (!altitude.empty())
                std::printf("[drive] %-8s altitude au-dessus du sol : min %d, mediane %d, 90 %% %d, max %d\n", vehicle.c_str(),
                            altitude.front(), altitude[altitude.size() / 2], altitude[altitude.size() * 9 / 10], altitude.back());
            std::printf("[drive] %-8s PAS de tour en %d ticks : %lld/%d echantillons parcourus, %d ALT, %d ticks contre le decor ; "
                        "derniere ligne %d sur %zu, %d controle(s) manquant(s)%s\n",
                        vehicle.c_str(), kMaxTicks, static_cast<long long>(travelled), n, recovers, contact_ticks,
                        p.last_check == 0x80 ? -1 : (p.last_check & 0x7F), session.race().check_lines().size(),
                        p.checks_missing, p.missed_check ? ", MISSED CHECK" : "");
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::vector<std::string> pos;
    std::string shot, top, dump;   // -DUMP=DIR: every generated file written there, for analysis
    int shot_s = 2;
    bool no_window = false, analyse = false, start_on_grid = false, drive = false;
    std::string real;   // -REAL=LEVEL: a shipped circuit instead, to compare with
    bool repaint = false;
    bool corridor = false;   // -CORRIDOR: the old method (the donor's landscape carried along)
    std::vector<std::array<int, 6>> paints;   // M, Z0, X0, Z1, X1, R (a disc: Z1 = Z0, X1 = X0)
    std::string autotile;
    int autotile_min = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        std::string u = a;
        for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u.rfind("-SHOT=", 0) == 0) shot = a.substr(6);
        else if (u.rfind("-SHOTAT=", 0) == 0) shot_s = std::max(1, std::atoi(a.c_str() + 8));
        else if (u.rfind("-TOP=", 0) == 0) top = a.substr(5);
        else if (u.rfind("-DUMP=", 0) == 0) dump = a.substr(6);
        else if (u == "-NOWINDOW") no_window = true;
        else if (u == "-CAM=GRID") start_on_grid = true;
        else if (u == "-DRIVE") drive = true;
        else if (u.rfind("-REAL=", 0) == 0) real = u.substr(6);
        else if (u == "--ANALYSE" || u == "--ANALYZE") analyse = true;
        else if (u == "-REPAINT") repaint = true;
        else if (u == "-CORRIDOR") corridor = true;
        else if (u.rfind("-PAINT=", 0) == 0 || u.rfind("-PAINTLINE=", 0) == 0) {
            const bool line = u.rfind("-PAINTLINE=", 0) == 0;
            std::array<int, 6> p{};
            int n = 0;
            for (const char* s = a.c_str() + (line ? 11 : 7); *s && n < 6; ++n) {
                p[static_cast<size_t>(n)] = std::atoi(s);
                while (*s && *s != ',') ++s;
                if (*s == ',') ++s;
            }
            if (!line) p = {p[0], p[1], p[2], p[1], p[2], p[3]};
            paints.push_back(p);
        }
        else if (u.rfind("--AUTOTILE=", 0) == 0) autotile = u.substr(11);
        else if (u.rfind("-MIN=", 0) == 0) autotile_min = std::atoi(a.c_str() + 5);
        else pos.push_back(a);
    }
    const std::string game_dir = (!pos.empty() && pos[0] != "-") ? pos[0] : bigred::find_game_dir();
    std::string theme = pos.size() > 1 ? pos[1] : "DIRT";
    uint64_t seed = pos.size() > 2 ? std::strtoull(pos[2].c_str(), nullptr, 10) : 1;

    if (!autotile.empty()) {
        int failed = 0;
        size_t from = 0;
        while (from <= autotile.size()) {
            const size_t comma = autotile.find(',', from);
            const std::string lv = autotile.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
            from = comma == std::string::npos ? autotile.size() + 1 : comma + 1;
            if (lv.empty()) continue;
            try {
                const bigred::ArchivePtr a = bigred::game_archive(game_dir, lv + ".LEV");
                const bigred::gen::AutoTiles at = bigred::gen::learn_autotiles(*a, lv);
                const bool ok = at.valid() && at.round_trip_permille >= autotile_min;
                failed += ok ? 0 : 1;
                std::printf("[autotile] %s%s\n", at.valid() ? at.report.c_str() : (lv + " : illisible").c_str(), ok ? "" : "  <- SOUS LE SEUIL");
            } catch (const std::exception& e) {
                ++failed;
                std::printf("[autotile] %s : %s\n", lv.c_str(), e.what());
            }
        }
        return failed ? 1 : 0;
    }

    if (analyse) {
        for (const bigred::gen::Theme& th : bigred::gen::themes())
            for (const std::string& lv : th.levels) {
                try {
                    const bigred::ArchivePtr a = bigred::game_archive(game_dir, lv + ".LEV");
                    std::printf("[%s] %s", th.name.c_str(), bigred::gen::analyse_donor(*a, lv).report.c_str());
                } catch (const std::exception& e) {
                    std::printf("[%s] %s : %s\n", th.name.c_str(), lv.c_str(), e.what());
                }
            }
        return 0;
    }

    int theme_index = 0;
    for (size_t i = 0; i < bigred::gen::themes().size(); ++i)
        if (bigred::gen::themes()[i].name == theme) theme_index = static_cast<int>(i);

    bigred::gen::Track track;
    auto generate = [&]() -> bool {
        try {
            if (!real.empty()) {
                // A SHIPPED circuit dressed as a Track, so everything below - the grid view, the
                // view from above, the autopilot - shows it the same way as a generated one.
                const bigred::ArchivePtr a = bigred::game_archive(game_dir, real + ".LEV");
                track = bigred::gen::Track();
                track.name = real;
                track.theme = real;
                track.files = *a;
                track.donor = bigred::gen::analyse_donor(*a, real);
                if (const bigred::Blob* cse = bigred::find_member(*a, "MAP.CSE"))
                    track.gates = bigred::parse_gates(cse->data(), cse->size());
                for (const bigred::Gate& g : track.gates) {
                    const int32_t ang = (static_cast<int32_t>(g.angle) & 0x3FF) << 6;
                    track.centre.push_back({g.x + bigred::fx::mul15(g.length / 2, bigred::fx::sin16(ang)),
                                            g.z + bigred::fx::mul15(g.length / 2, bigred::fx::cos16(ang))});
                }
                for (const std::string& c : bigred::parse_car_list(std::string(
                         a->at(real + ".TXT").begin(), a->at(real + ".TXT").end())))
                    if (!bigred::is_presentation_vehicle(c)) track.cars.push_back(c);
                track.report = real + " (circuit d'origine)";
                if (!paints.empty()) {
                    // An editor's brush: grounds painted on the vertices, the rest repaired
                    // round them, then the tiles laid.
                    const bigred::gen::AutoTiles at = bigred::gen::learn_autotiles(*a, real);
                    for (int m = 0; m < at.materials; ++m) {
                        int code = -1;
                        for (int c = 0; c < 256 && code < 0; ++c)
                            if (at.plain_of[static_cast<size_t>(c)] == m) code = c;
                        std::printf("[paint] sol %d : couleur %d,%d,%d, code %d%s\n", m, at.colour[static_cast<size_t>(m)][0],
                                    at.colour[static_cast<size_t>(m)][1], at.colour[static_cast<size_t>(m)][2], code,
                                    at.wet[static_cast<size_t>(m)] ? " (eau)" : "");
                    }
                    bigred::Blob& mpc = track.files.at("MAP.MPC");
                    std::vector<int8_t> V = bigred::gen::vertex_materials(at, mpc);
                    std::vector<uint8_t> locked(V.size(), 0);
                    int painted = 0;
                    for (const auto& p : paints) {
                        const int m = p[0], r = p[5];
                        if (m < 0 || m >= at.materials) continue;
                        const int64_t dz = p[3] - p[1], dx = p[4] - p[2], len2 = dz * dz + dx * dx;
                        for (int z = 0; z <= 256; ++z)
                            for (int x = 0; x <= 256; ++x) {
                                // distance from the vertex to the stroke's segment, squared, x 1
                                int64_t t = len2 ? ((z - p[1]) * dz + (x - p[2]) * dx) : 0;
                                t = std::clamp<int64_t>(t, 0, len2);
                                const int64_t qz = len2 ? p[1] * len2 + t * dz : p[1], qx = len2 ? p[2] * len2 + t * dx : p[2];
                                const int64_t ez = len2 ? z * len2 - qz : z - qz, ex = len2 ? x * len2 - qx : x - qx;
                                const int64_t scale = len2 ? len2 : 1;
                                if (ez * ez + ex * ex <= static_cast<int64_t>(r) * r * scale * scale) {
                                    V[static_cast<size_t>(z) * 257 + x] = static_cast<int8_t>(m);
                                    locked[static_cast<size_t>(z) * 257 + x] = 1;
                                    ++painted;
                                }
                            }
                    }
                    const int repaired = bigred::gen::repair_materials(at, V, locked);
                    const int laid = bigred::gen::lay_autotiles(at, V, mpc);
                    track.report += ", PEINT : " + std::to_string(painted) + " sommets peints, " + std::to_string(repaired) +
                                    " repares autour, " + std::to_string(laid) + " cases reposees";
                }
                if (repaint) {
                    // The shipped ground, laid again from its vertex materials alone.
                    const bigred::gen::AutoTiles at = bigred::gen::learn_autotiles(*a, real);
                    bigred::Blob& mpc = track.files.at("MAP.MPC");
                    const bigred::Blob before = mpc;
                    const int changed = bigred::gen::lay_autotiles(at, bigred::gen::vertex_materials(at, before), mpc);
                    track.report += ", REPEINT : " + at.report + ", " + std::to_string(changed) + " cases differentes";
                }
                std::printf("[gen] %s\n", track.report.c_str());
                return true;
            }
            track = bigred::gen::generate(game_dir, theme, seed, "GEN",
                                          corridor ? bigred::gen::Method::Corridor : bigred::gen::Method::Layers);
            std::printf("[gen] %s\n", track.report.c_str());
            return true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[gen] %s %llu : %s\n", theme.c_str(),
                         static_cast<unsigned long long>(seed), e.what());
            return false;
        }
    };
    if (!generate()) return 1;
    if (!dump.empty()) {
        int written = 0;
        for (const auto& [name, blob] : track.files)
            if (FILE* f = std::fopen((dump + "/" + name).c_str(), "wb")) {
                written += std::fwrite(blob.data(), 1, blob.size(), f) == blob.size() ? 1 : 0;
                std::fclose(f);
            }
        std::printf("%d file(s) written to %s\n", written, dump.c_str());
    }
    if (!top.empty()) std::printf("%s %s\n", write_top_png(track, top) ? "Top view written to" : "Top view FAILED:", top.c_str());
    bigred::LevelScene scene;
    scene.load_shade_table(game_dir);
    auto load_scene = [&]() {
        return scene.load_files(track.name, track.files, track.map_w, track.map_h,
                                track.theme + " " + std::to_string(track.seed));
    };
    if (drive) {
        if (!load_scene()) return 1;
        drive_test(game_dir, track, scene);
    }
    if (no_window) return 0;
    if (!drive && !load_scene()) return 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window* window = SDL_CreateWindow("OpenBigRed", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) return 1;
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context || !gl::load()) return 1;
    SDL_GL_SetSwapInterval(1);

    bigred::WorldRenderer renderer;
    if (!renderer.create()) return 1;
    renderer.upload(scene);

    // The ring and the CHECKP lines, as map_preview -DEBUG draws them for a shipped level.
    bool show_ring = true;
    std::vector<bigred::TerrainVertex> ring;
    auto build_ring = [&]() {
        ring.clear();
        std::vector<bigred::Gate> gates = track.gates;
        std::vector<bigred::CheckLine> lines = bigred::build_check_lines(
            scene.instances, scene.shape_names_by_slot,
            [&](const std::string& shape) -> const bigred::Blob* {
                auto it = scene.lev.find(shape + ".FT");
                return it == scene.lev.end() ? nullptr : &it->second;
            });
        bigred::mark_checkpoint_gates(gates, lines);
        bigred::SimTerrain ter;
        ter.mph = scene.mph_data;
        ter.mpc = scene.mpc_data;
        ter.cht = (scene.cht_size >= 512) ? scene.cht_data : nullptr;
        ter.w = scene.map_w;
        ter.h = scene.map_h;
        ring = bigred::build_gate_overlay(gates, ter, scene.map_w, kRenderScale);
        const auto masts = bigred::build_check_line_overlay(lines, ter, scene.map_w, kRenderScale);
        ring.insert(ring.end(), masts.begin(), masts.end());
        int in_gate = 0;
        for (const bigred::CheckLine& l : lines) in_gate += l.in_gate ? 1 : 0;
        std::printf("[gen] %zu lignes CHECKP, %d dans une porte\n", lines.size(), in_gate);
    };
    build_ring();
    auto set_title = [&]() {
        const std::string t = "OpenBigRed - track_gen " + track.theme + " " + std::to_string(track.seed);
        SDL_SetWindowTitle(window, t.c_str());
    };
    set_title();

    glEnable(GL_DEPTH_TEST);
    const float span = static_cast<float>(scene.map_w * bigred::kTerrainTexelWorldSize) * kRenderScale;
    Mat4::Vec3 cam{span * 0.5f, span * 0.55f, span * -0.05f};
    float pitch = -0.95f, yaw = 0.0f, speed = span * 0.15f;
    bool mouselook = false;
    int width = 1280, height = 720;
    Uint32 last = SDL_GetTicks();
    const Uint32 shot_at = last + static_cast<Uint32>(shot_s) * 1000u;

    // The view a driver has on the grid: behind the line, a car's height up, along the road.
    auto grid_camera = [&]() {
        const bigred::Blob& txt = track.files.at(track.name + ".TXT");
        const bigred::StartingGrid g = bigred::parse_grid(std::string(txt.begin(), txt.end()));
        const bigred::RespawnPoint p = bigred::grid_slot(g, 6, 0);
        const int gx = std::clamp(p.x / 512, 0, scene.map_w - 1), gz = std::clamp(p.z / 512, 0, scene.map_h - 1);
        const float ground = static_cast<float>(scene.mph_data[static_cast<size_t>(gz) * scene.map_w + gx]) * 16.0f;
        const float t = static_cast<float>(p.yaw_bam16) * 6.283185307f / 65536.0f;
        // Game (x, z) along (sin t, cos t); render X is mirrored, so the look is (-sin t, cos t).
        const float back = 1500.0f;
        const float gxw = static_cast<float>(p.x) - std::sin(t) * back, gzw = static_cast<float>(p.z) - std::cos(t) * back;
        cam = {(static_cast<float>(scene.map_w - 1) * 512.0f - gxw) * kRenderScale,
               (ground + 700.0f) * kRenderScale, gzw * kRenderScale};
        yaw = -t;
        pitch = -0.12f;
        speed = span * 0.03f;
    };
    if (start_on_grid) grid_camera();

    auto regenerate = [&]() {
        if (!generate() || !load_scene()) return;
        renderer.upload(scene);
        build_ring();
        set_title();
        if (start_on_grid) grid_camera();
    };

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                width = ev.window.data1;
                height = ev.window.data2;
                glViewport(0, 0, width, height);
            } else if (ev.type == SDL_KEYDOWN) {
                const SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE) running = false;
                else if (k == SDLK_n) { ++seed; regenerate(); }
                else if (k == SDLK_p) { if (seed > 0) --seed; regenerate(); }
                else if (k == SDLK_r) { seed = (static_cast<uint64_t>(SDL_GetTicks()) * 2654435761u) % 1000000; regenerate(); }
                else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
                    const int n = static_cast<int>(bigred::gen::themes().size());
                    theme_index = (theme_index + (k == SDLK_PAGEUP ? 1 : n - 1)) % n;
                    theme = bigred::gen::themes()[static_cast<size_t>(theme_index)].name;
                    regenerate();
                } else if (k == SDLK_t) {
                    std::printf("%s track_top.bmp\n", write_top_png(track, "track_top.png") ? "Top view written to" : "Top view FAILED:");
                } else if (k == SDLK_c) show_ring = !show_ring;
                else if (k == SDLK_g) { start_on_grid = true; grid_camera(); }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_RIGHT) {
                mouselook = true;
                SDL_SetRelativeMouseMode(SDL_TRUE);
            } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_RIGHT) {
                mouselook = false;
                SDL_SetRelativeMouseMode(SDL_FALSE);
            } else if (ev.type == SDL_MOUSEMOTION && mouselook) {
                yaw -= static_cast<float>(ev.motion.xrel) * 0.0025f;
                pitch = std::clamp(pitch - static_cast<float>(ev.motion.yrel) * 0.0025f, -1.55f, 1.55f);
            } else if (ev.type == SDL_MOUSEWHEEL) {
                speed = std::clamp(speed * std::pow(1.15f, static_cast<float>(ev.wheel.y)), span * 0.01f, span * 2.0f);
            }
        }
        const Uint32 now = SDL_GetTicks();
        const float dt = static_cast<float>(now - last) / 1000.0f;
        last = now;
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        const Mat4::Vec3 look{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
        const Mat4::Vec3 right = Mat4::normalize(Mat4::cross(look, {0.0f, 1.0f, 0.0f}));
        const float sgn_fb = keys[SDL_SCANCODE_UP] ? 1.0f : keys[SDL_SCANCODE_DOWN] ? -1.0f : 0.0f;
        const float sgn_lr = keys[SDL_SCANCODE_RIGHT] ? 1.0f : keys[SDL_SCANCODE_LEFT] ? -1.0f : 0.0f;
        cam.x += (look.x * sgn_fb + right.x * sgn_lr) * speed * dt;
        cam.y += look.y * sgn_fb * speed * dt;
        cam.z += (look.z * sgn_fb + right.z * sgn_lr) * speed * dt;

        const Mat4 view = Mat4::lookAt(cam, {cam.x + look.x, cam.y + look.y, cam.z + look.z}, {0.0f, 1.0f, 0.0f});
        const Mat4 proj = Mat4::perspective(60.0f * 3.14159265f / 180.0f,
                                            static_cast<float>(width) / static_cast<float>(height),
                                            bigred::kNearPlaneWorld * kRenderScale, span * 4.0f);
        bigred::WorldFrame frame;
        frame.view.mvp = proj * view * Mat4::identity();
        frame.view.eye = cam;
        frame.view.forward = look;
        frame.view.right = right;
        frame.view.up = Mat4::normalize(Mat4::cross(right, look));
        frame.view.aspect = static_cast<float>(width) / static_cast<float>(height);
        frame.wall_ms = now;
        if (show_ring) frame.lines = &ring;
        renderer.draw(scene, frame);

        if (!shot.empty() && SDL_GetTicks() >= shot_at) {
            std::vector<uint8_t> px(static_cast<size_t>(width) * height * 4);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ABGR8888);
            if (s) {
                for (int y = 0; y < height; ++y)
                    std::memcpy(static_cast<uint8_t*>(s->pixels) + static_cast<size_t>(y) * s->pitch,
                                px.data() + static_cast<size_t>(height - 1 - y) * width * 4,
                                static_cast<size_t>(width) * 4);
                std::printf("%s %s\n", SDL_SaveBMP(s, shot.c_str()) == 0 ? "Shot written to" : "Shot FAILED:", shot.c_str());
                SDL_FreeSurface(s);
            }
            running = false;
        }
        SDL_GL_SwapWindow(window);
    }
    renderer.release();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
