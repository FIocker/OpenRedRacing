#include "menu_shots.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/FontEngineInterface.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <vector>

#include "app/app.h"
#include "game/competition.h"
#include "formats/bigred_archive.h"
#include "game/car_screen.h"
#include "game/championship_screen.h"
#include "game/character_screen.h"
#include "game/globe_screen.h"
#include "game/level_catalog.h"
#include "game/lobby_screen.h"
#include "game/main_menu_screen.h"
#include "game/multiplayer_screen.h"
#include "game/options_screen.h"
#include "game/placeholder_screen.h"
#include "game/profile_screen.h"
#include "game/results_screen.h"
#include "game/split_screen.h"
#include "game/track_screen.h"
#include "net/net_game.h"
#include "ui/team_logos.h"

namespace bigred {
namespace {

// The window sizes each screen is shot at: the default window, full HD, and two small 4:3 ones
// where a layout written for 720 lines is most likely to break.
struct WindowSize {
    int w, h;
};
constexpr WindowSize kSizes[] = {{1280, 720}, {1920, 1080}, {1024, 768}, {800, 600}};
constexpr int kSizeCount = static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));
// Frames a screen is left to settle before it is read back: RmlUi lays out on the first
// update, the vehicle screen builds its model, the globe turns into place.
constexpr int kSettleFrames = 8;

struct Shot {
    const char* name;
    std::function<std::unique_ptr<Screen>(App&)> make;
    // Frames to wait before reading it back: longer for a screen that waits on something -
    // the list of games has to hear a host answer.
    int settle = kSettleFrames;
};

// The circuit every level-bound screen is shown with, and the selection the vehicle screen
// writes into - static because the screen keeps the pointer.
int g_car_selection = 0;

// The n-th circuit that can be raced: the first, and a few others whose pictures put their
// title banner elsewhere.
LevelEntry nth_level(App& app, int n) {
    LevelEntry last;
    for (LevelEntry& l : scan_levels(app.game_dir()))
        if (!l.cars.empty()) {
            last = l;
            if (n-- == 0) return l;
        }
    return last;
}
LevelEntry first_level(App& app) { return nth_level(app, 0); }

std::vector<Shot> shots() {
    return {
        {"main_menu", [](App&) { return std::make_unique<MainMenuScreen>(); }},
        {"profile_create", [](App&) { return std::make_unique<ProfileScreen>(ProfileScreen::Mode::Create); }},
        {"profile_edit", [](App& app) {
             // Past 9: the counter that used to wrap onto two lines.
             app.profile().logo_index = 9;
             return std::make_unique<ProfileScreen>(ProfileScreen::Mode::Edit);
         }},
        {"character_1", [](App& app) { app.profile().character = 0; return std::make_unique<CharacterScreen>(nullptr); }},
        {"character_2", [](App& app) { app.profile().character = 1; return std::make_unique<CharacterScreen>(nullptr); }},
        {"character_3", [](App& app) { app.profile().character = 2; return std::make_unique<CharacterScreen>(nullptr); }},
        {"character_4", [](App& app) { app.profile().character = 3; return std::make_unique<CharacterScreen>(nullptr); }},
        {"character_5", [](App& app) { app.profile().character = 4; return std::make_unique<CharacterScreen>(nullptr); }},
        {"character_6", [](App& app) { app.profile().character = 5; return std::make_unique<CharacterScreen>(nullptr); }},
        {"options", [](App&) { return std::make_unique<OptionsScreen>(); }},
        {"options_display", [](App&) { return std::make_unique<OptionsDisplayScreen>(); }},
        {"options_sound", [](App&) { return std::make_unique<OptionsSoundScreen>(); }},
        {"multiplayer", [](App&) { return std::make_unique<MultiplayerScreen>(); }},
        {"network", [](App&) { return std::make_unique<NetworkScreen>(); }},
        {"join", [](App&) { return std::make_unique<JoinScreen>(); }},
        {"globe", [](App&) { return std::make_unique<GlobeScreen>(); }},
        {"track", [](App& app) { return std::make_unique<TrackScreen>(first_level(app)); }},
        {"track_7", [](App& app) { return std::make_unique<TrackScreen>(nth_level(app, 7)); }},
        {"track_15", [](App& app) { return std::make_unique<TrackScreen>(nth_level(app, 15)); }},
        {"track_21", [](App& app) { return std::make_unique<TrackScreen>(nth_level(app, 21)); }},
        // The last circuit of the list - an added one when `--shots` is given `-ADDLEVEL=`, whose
        // picture then comes out of its in-memory files (formats/game_files.h, add_game_archive).
        {"track_last", [](App& app) { return std::make_unique<TrackScreen>(nth_level(app, 1 << 20)); }},
        {"car", [](App& app) { return std::make_unique<CarScreen>(first_level(app), &g_car_selection); }},
        // Open-topped: the driver's head shows (the `&40` anchor).
        {"car_jeep", [](App& app) {
             LevelEntry l = nth_level(app, 0);
             l.cars = {"JEEP", "BEETLE", "ATV"};
             return std::make_unique<CarScreen>(l, &g_car_selection);
         }},
        // The player's own logo (ui/team_logos.h), when %APPDATA%\OpenBigRed\logo.png is there;
        // without it the index wraps onto the game's first.
        {"car_own_logo", [](App& app) {
             app.profile().logo_index = kTeamLogoCount;
             return std::make_unique<CarScreen>(first_level(app), &g_car_selection);
         }},
        {"controls", [](App&) { return std::make_unique<OptionsControlsScreen>(); }},
        {"championship", [](App&) { return std::make_unique<ChampionshipScreen>(); }},
        // A championship round on the globe: three circuits only.
        {"globe_round", [](App&) {
             return std::make_unique<GlobeScreen>(std::vector<std::string>{"SNOW1", "DIRT1", "CITY1"});
         }},
        {"results", [](App&) {
             ResultsScreen::Setup s;
             s.level_title = "UNITED KINGDOM";
             s.laps = 5;
             s.place = 3;
             s.finish_ticks = 14000;
             for (int i = 0; i < 5; ++i) s.lap_ticks.push_back(2700u + static_cast<uint32_t>(i) * 37u);
             const char* names[] = {"ALEXANDRE", "BOB", "CHARLOTTE", "DAVE", "EMMANUELLE", "FRED", "GASPARD", "HELENE", "IRIS", "JULES", "KARIMA", "LEONARDO"};
             s.heading = "RACE 2 OF 3";
             for (int i = 0; i < 12; ++i) {
                 ResultsScreen::Setup::Row r;
                 r.place = i + 1;
                 r.name = names[i];
                 r.finish_ticks = 13500u + static_cast<uint32_t>(i) * 180u;
                 r.status = i == 10 ? ResultsScreen::Setup::Row::Status::Wrecked
                          : i == 11 ? ResultsScreen::Setup::Row::Status::Running
                                    : ResultsScreen::Setup::Row::Status::Finished;
                 r.local = i == 2;
                 r.race_points = i < 6 ? 10 - 2 * i : 0;
                 r.total_points = r.race_points + 12 - i;
                 s.rows.push_back(r);
             }
             // A series in progress: RACE AGAIN and QUIT SERIES beside the OK.
             s.replay = []() -> std::unique_ptr<Screen> { return nullptr; };
             s.quit_label = "QUIT SERIES";
             return std::make_unique<ResultsScreen>(s);
         }},
        {"placeholder", [](App&) {
             return std::make_unique<PlaceholderScreen>("CONTROLS", "NOT BUILT YET - COMING LATER");
         }},
        {"split_screen", [](App&) { return std::make_unique<SplitScreenScreen>(); }},
        // Three joined, the first one ready, each on another line of the sheet.
        {"split_screen_3", [](App&) { return std::make_unique<SplitScreenScreen>(3); }},
        {"split_car", [](App& app) -> std::unique_ptr<Screen> {
             app.locals() = default_local_players(app, 3);
             LevelEntry l = nth_level(app, 0);
             l.cars = {"JEEP", "BEETLE", "ATV"};
             return std::make_unique<SplitCarScreen>(l, nullptr);
         }},
        // A split screen choosing a series of three on the globe, two already in: the list
        // under CHOSEN, in the order they were chosen.
        {"globe_series", [](App& app) -> std::unique_ptr<Screen> {
             app.locals() = default_local_players(app, 2);
             app.competition().start_series(2, 3, false);
             app.competition().series().circuits = {nth_level(app, 7).name, nth_level(app, 15).name};
             return std::make_unique<GlobeScreen>();
         }},
        {"lobby", [](App& app) -> std::unique_ptr<Screen> {
             NetSettings s;
             s.level = first_level(app).name;
             // Loopback only: a screenshot must not announce a game to the LAN.
             if (!app.net().host(look_from_profile(app), s, /*loopback_only=*/true)) return nullptr;
             return std::make_unique<LobbyScreen>();
         }},
        // The host changing the circuit (user, 2026-09-27): other circuits, other names, other
        // player caps - the settings column must hold whatever it shows.
        {"lobby_7", [](App& app) -> std::unique_ptr<Screen> {
             NetSettings s;
             s.level = nth_level(app, 7).name;
             if (!app.net().host(look_from_profile(app), s, true)) return nullptr;
             return std::make_unique<LobbyScreen>();
         }},
        {"lobby_15", [](App& app) -> std::unique_ptr<Screen> {
             NetSettings s;
             s.level = nth_level(app, 15).name;
             s.races = 3;
             if (!app.net().host(look_from_profile(app), s, true)) return nullptr;
             return std::make_unique<LobbyScreen>();
         }},
        // LAST, because it leaves a game open: a host on the loopback, and the list of games
        // that finds it - the search as a player would see it.
        {"join_found", [](App& app) -> std::unique_ptr<Screen> {
             NetSettings s;
             s.level = nth_level(app, 21).name;
             if (!app.net().host(look_from_profile(app), s, true)) return nullptr;
             return std::make_unique<JoinScreen>(std::string(), LobbyScreen::Auto{}, true);
         }, 240},
    };
}

// --- PNG, stored (uncompressed) deflate: no library for a debugging tool ---------------------

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xffffffffu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        ready = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c;
}

void put32(Blob& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}

void chunk(Blob& out, const char* type, const Blob& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    Blob body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()) ^ 0xffffffffu);
}

Blob encode_png(const std::vector<uint8_t>& rgba, int w, int h) {
    Blob raw;   // filter byte 0 + RGB per row
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 3));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const size_t o = (static_cast<size_t>(y) * w + x) * 4;
            raw.insert(raw.end(), {rgba[o], rgba[o + 1], rgba[o + 2]});
        }
    }
    Blob z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.insert(z.end(), {static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8),
                           static_cast<uint8_t>(~n), static_cast<uint8_t>(~n >> 8)});
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
        pos += n;
        if (last) break;
    }
    put32(z, (b << 16) | a);

    Blob png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    Blob ihdr;
    put32(ihdr, static_cast<uint32_t>(w));
    put32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8-bit RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

// --- the layout check -------------------------------------------------------------------------

struct Item {
    float x0, y0, x1, y1;
    Rml::Element* el;        // the element that owns the box (the text node for a text line)
    std::string what;
};

std::string describe(Rml::Element* el) {
    std::string s;
    for (Rml::Element* e = el; e && s.size() < 80; e = e->GetParentNode()) {
        const std::string a = e->GetAddress(false, false);
        if (a.rfind("#text", 0) == 0) continue;
        s = s.empty() ? a : a + " > " + s;
        if (!e->GetId().empty()) break;   // an id is enough to find it
    }
    return s;
}

bool is_ancestor(Rml::Element* a, Rml::Element* b) {
    for (Rml::Element* e = b; e; e = e->GetParentNode())
        if (e == a) return true;
    return false;
}

// The box a line of text is laid out in: the nearest ancestor that is not inline. Two lines
// with different baselines in the same box means the box broke its content over two lines.
Rml::Element* line_box(Rml::Element* el) {
    for (Rml::Element* e = el->GetParentNode(); e; e = e->GetParentNode())
        if (e->GetComputedValues().display() != Rml::Style::Display::Inline) return e;
    return el;
}

// The nearest ancestor with an id - "the widget" a thing belongs to.
Rml::Element* widget_of(Rml::Element* el) {
    for (Rml::Element* e = el; e; e = e->GetParentNode())
        if (!e->GetId().empty()) return e;
    return nullptr;
}

struct TextLine {
    Rml::Element* box;
    float baseline;
    std::string text;
};

void collect(Rml::Element* el, float cw, float ch, std::vector<Item>& items,
             std::vector<TextLine>& lines_out) {
    if (!el->IsVisible()) return;
    if (auto* text = dynamic_cast<Rml::ElementText*>(el)) {
        const Rml::FontMetrics& m = Rml::GetFontEngineInterface()->GetFontMetrics(el->GetFontFaceHandle());
        const Rml::Vector2f o = el->GetAbsoluteOffset();
        for (const auto& line : text->GetLines()) {
            if (line.width <= 0) continue;
            items.push_back({o.x + line.position.x, o.y + line.position.y - m.ascent,
                             o.x + line.position.x + static_cast<float>(line.width),
                             o.y + line.position.y + m.descent, el, "\"" + line.text + "\""});
            lines_out.push_back({line_box(el), o.y + line.position.y, line.text});
        }
        return;
    }
    const Rml::ComputedValues& cv = el->GetComputedValues();
    const bool drawn = el->GetTagName() == "img" || cv.background_color().alpha > 0 ||
                       cv.border_top_width() > 0.0f;
    if (drawn) {
        const Rml::Vector2f o = el->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f s = el->GetBox().GetSize(Rml::BoxArea::Border);
        // A backdrop - the screen's own background picture, a panel - is meant to have things
        // on it.
        if (s.x > 0 && s.y > 0 && s.x * s.y < 0.5f * cw * ch)
            items.push_back({o.x, o.y, o.x + s.x, o.y + s.y, el, "[" + el->GetTagName() + "]"});
    }
    for (int i = 0; i < el->GetNumChildren(); ++i) collect(el->GetChild(i), cw, ch, items, lines_out);
}

std::string check_layout(Rml::ElementDocument* doc, int cw, int ch, int& issues) {
    std::vector<Item> items;
    std::vector<TextLine> lines;
    collect(doc, static_cast<float>(cw), static_cast<float>(ch), items, lines);
    std::string out;
    char buf[64];
    for (const Item& it : items) {
        if (it.x0 < -1 || it.y0 < -1 || it.x1 > cw + 1 || it.y1 > ch + 1) {
            std::snprintf(buf, sizeof buf, " (%.0f,%.0f)-(%.0f,%.0f)", it.x0, it.y0, it.x1, it.y1);
            out += "  OUTSIDE  " + describe(it.el) + " " + it.what + buf + "\n";
            ++issues;
        }
    }
    for (size_t i = 0; i < items.size(); ++i)
        for (size_t j = i + 1; j < items.size(); ++j) {
            const Item& a = items[i];
            const Item& b = items[j];
            if (is_ancestor(a.el, b.el) || is_ancestor(b.el, a.el)) continue;
            // Two lines of the same text are one thing, laid out by the same rule.
            if (a.el == b.el) continue;
            // A picture inside the same widget as what crosses it is a frame or a backdrop on
            // purpose: the track screen's button plates, the logo on its plaque.
            if ((a.what == "[img]" || b.what == "[img]") && widget_of(a.el) == widget_of(b.el)) continue;
            // One of the ORIGINAL'S sprites (ui/scn_kit.h, class `spr`): the screen is the
            // original's own composition - a value written on its bar, a title brushing a button -
            // and only a text crossing a text is ours to catch.
            if (a.el->IsClassSet("spr") || b.el->IsClassSet("spr")) continue;
            const float ox = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
            const float oy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
            if (ox < 2 || oy < 2) continue;
            std::snprintf(buf, sizeof buf, " by %.0fx%.0f", ox, oy);
            out += "  OVERLAP  " + describe(a.el) + " " + a.what + "  x  " + describe(b.el) + " " +
                   b.what + buf + "\n";
            ++issues;
        }
    // A box whose text sits on more than one baseline wrapped. Text meant to run over several
    // lines - a description, a note - carries the class `wrap-ok` and is let through.
    std::vector<Rml::Element*> seen;
    for (size_t i = 0; i < lines.size(); ++i) {
        Rml::Element* box = lines[i].box;
        if (std::find(seen.begin(), seen.end(), box) != seen.end()) continue;
        seen.push_back(box);
        bool ok = false;
        for (Rml::Element* e = box; e && !ok; e = e->GetParentNode()) ok = e->IsClassSet("wrap-ok");
        if (ok) continue;
        std::vector<float> baselines;
        std::string all;
        for (size_t j = i; j < lines.size(); ++j) {
            if (lines[j].box != box) continue;
            bool known = false;
            for (float y : baselines) known = known || std::abs(y - lines[j].baseline) < 2.0f;
            if (!known) {
                baselines.push_back(lines[j].baseline);
                if (!all.empty()) all += " |";
            }
            all += " " + lines[j].text;
        }
        if (baselines.size() > 1) {
            out += "  WRAP     " + describe(box) + " on " + std::to_string(baselines.size()) +
                   " lines:" + all + "\n";
            ++issues;
        }
    }
    return out;
}

} // namespace

bool write_png(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    if (w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return false;
    return write_file_utf8(path, encode_png(rgba, w, h));
}

bool MenuShots::enter(App& app) {
    create_directories_utf8(out_dir_);
    app.set_frame_hook([this](App& a) { on_frame(a); });
    return true;
}

void MenuShots::draw(App&) {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

// OBR_SHOTS_FILTER=word keeps only the screens whose name holds it; OBR_SHOTS_ONE=1 shoots the
// default window size alone. For working on one screen without waiting for all of them.
std::vector<Shot> filtered_shots() {
    std::vector<Shot> all = shots();
    const char* f = std::getenv("OBR_SHOTS_FILTER");
    if (!f || !*f) return all;
    std::vector<Shot> kept;
    for (Shot& s : all)
        if (std::string(s.name).find(f) != std::string::npos) kept.push_back(std::move(s));
    return kept;
}
int size_count() { return std::getenv("OBR_SHOTS_ONE") ? 1 : kSizeCount; }

void MenuShots::on_frame(App& app) {
    const std::vector<Shot> list = filtered_shots();
    if (frames_ < 0) {
        if (shot_ >= list.size()) {
            ++size_;
            shot_ = 0;
        }
        if (size_ >= static_cast<size_t>(size_count())) {
            write_file_utf8(out_dir_ + "/report.txt", Blob(report_.begin(), report_.end()));
            std::printf("%s\n%d issue(s). PNGs and report.txt in %s\n", report_.c_str(), issues_,
                        out_dir_.c_str());
            std::fflush(stdout);
            app.set_frame_hook(nullptr);
            app.quit();
            return;
        }
        const WindowSize ws = kSizes[size_];
        if (app.width() != ws.w || app.height() != ws.h) app.resize_window(ws.w, ws.h);
        std::unique_ptr<Screen> s = list[shot_].make(app);
        if (!s) {
            report_ += std::string(list[shot_].name) + ": could not be opened\n";
            ++shot_;
            return;
        }
        app.push(std::move(s));
        frames_ = 0;
        return;
    }
    if (++frames_ < list[shot_].settle) return;

    const WindowSize ws = kSizes[size_];
    const std::string tag = std::string(list[shot_].name) + "_" + std::to_string(ws.w) + "x" +
                            std::to_string(ws.h);
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (app.presentation().read_canvas(rgba, w, h))
        write_file_utf8(out_dir_ + "/" + tag + ".png", encode_png(rgba, w, h));

    // The top screen's context is the newest one: this tool pops each screen before the next.
    std::string found;
    if (const int n = Rml::GetNumContexts(); n > 0)
        if (Rml::Context* c = Rml::GetContext(n - 1))
            if (c->GetNumDocuments() > 0) found = check_layout(c->GetDocument(0), w, h, issues_);
    report_ += tag + " (canvas " + std::to_string(w) + "x" + std::to_string(h) + ")" +
               (found.empty() ? " ok\n" : "\n" + found);

    app.pop();
    ++shot_;
    frames_ = -1;
}

} // namespace bigred
