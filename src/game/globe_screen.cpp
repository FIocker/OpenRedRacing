#include "globe_screen.h"

#include <RmlUi/Core.h>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>

#include "app/app.h"
#include "game/competition.h"
#include "app/local_players.h"
#include "formats/game_files.h"
#include "formats/tourn.h"   // kFrontEndMusicTrack
#include "game/track_screen.h"

namespace bigred {
namespace {

// ⚠️ A DEBUG AFFORDANCE, AND DELIBERATELY NOT A KEY: `openbigred` is the game and carries
// none (CLAUDE.md). Set `OPENBIGRED_GLOBE_MARKERS=all` in the environment to pin every circuit
// on the globe at once, which is how the placement of all 21 was checked. Read once.
bool show_all_markers() {
    static const bool on = [] {
        const char* v = std::getenv("OPENBIGRED_GLOBE_MARKERS");
        return v != nullptr && std::string(v) == "all";
    }();
    return on;
}

// Where the sphere sits in the canvas, as fractions of it - the screen lays out in percentages
// everywhere else too, so the globe follows the window like the rest of the document does.
constexpr float kGlobeCentreX = 0.42f;
constexpr float kGlobeCentreY = 0.46f;
constexpr float kGlobeRadius = 0.30f;   // of the canvas HEIGHT

// How fast the globe turns on its own while nothing is chosen: one revolution in 25 seconds,
// which is slow enough to read and fast enough to be obviously alive.
constexpr float kIdleTurnsPerSecond = 0.04f;

// How fast it swings to a chosen country. A fraction of the remaining angle per millisecond,
// so it eases in - a linear slew reads as a machine, this reads as a globe being spun.
constexpr float kEasePerMs = 0.006f;

// ⚠️ A CIRCUIT NEAR A POLE WOULD TIP THE GLOBE ON ITS SIDE. SNOW1 is the Arctic and its
// coordinates are the pole exactly, so the honest pitch for it is a quarter turn - straight
// down onto the top of the sphere, where nothing else is readable. Clamped.
constexpr float kMaxPitch = 0.14f;

// Every string on this screen is uppercase: the game's own alphabet has no lowercase at all
// (src/formats/sprite.h). Sizes are in dp (app/rml_screen.h), rounded down by the font to its 8px
// artwork - the same convention as every other screen in game/.
constexpr const char* kDocument = R"(<rml>
<head>
<style>
body { width: 100%; height: 100%; font-family: bigred; color: #ffffff; }
/* The title is the original's own: GLB_SINGLE in the wibbly lettering at its `@X`, placed by
   ui/scn_kit.h on the whole 320x200 screen - so its holder covers the canvas, and lets the
   pointer through to the widgets. */
#title { position: absolute; left: 0; top: 0; width: 100%; height: 100%; pointer-events: none; }
/* The hand-written name of the chosen country, straight out of NAMES.TM, laid across the
   globe exactly as the original does. The 128-pixel cell is stretched to 44% of the canvas,
   which is the 40% of a 320-wide screen it covered when the artwork was drawn. */
#place { position: absolute; left: 20%; top: 48%; width: 44%; }
#place img { width: 100%; }
#chosen {
    position: absolute; left: 66%; top: 28%; width: 32%;
    font-size: 16dp; color: #a0c0ff;
}
#chosen_name { position: absolute; left: 66%; top: 34%; width: 32%; font-size: 24dp; color: #ffdf40; }
/* Choosing a series: the race OK would add, under the ones already in. */
#chosen_name .next { color: #a0a0a0; }
/* The circuit's one-line description. Pink, under the globe, like the capture. */
#blurb {
    position: absolute; left: 6%; top: 78%; width: 58%;
    font-size: 16dp; color: #ff90c8;
}
#prev { position: absolute; left: 4%; top: 86%; width: 8%; }
#next { position: absolute; left: 13%; top: 86%; width: 8%; }
#ok { position: absolute; left: 79%; top: 74%; width: 15%; }
#prev img, #next img, #ok img { width: 100%; }
#prev:hover img, #next:hover img, #ok:hover img,
#prev:focus img, #next:focus img, #ok:focus img { transform: scale(1.15); }
</style>
</head>
<body>
    <div id="title"></div>
    <div id="place"></div>
    <div id="chosen"></div>
    <div id="chosen_name" class="wrap-ok"></div>
    <div id="blurb" class="wrap-ok"></div>
    <div id="prev"></div>
    <div id="next"></div>
    <div id="ok"></div>
</body>
</rml>)";

GlobeBody body_of(const LevelEntry& e) {
    switch (e.planet) {
    case 1: return GlobeBody::Mars;
    case 2: return GlobeBody::Venus;
    case 3: return GlobeBody::Moon;
    default: return GlobeBody::Earth;
    }
}

// The shortest way round from `from` to `to`, both in turns.
float shortest_delta(float from, float to) {
    float d = to - from;
    d -= std::floor(d + 0.5f);
    return d;
}

} // namespace

const char* GlobeScreen::document_source() const { return kDocument; }

const LevelEntry* GlobeScreen::current() const {
    if (!chosen_ || levels_.empty()) return nullptr;
    return &levels_[static_cast<size_t>(level_)];
}

bool GlobeScreen::enter(App& app) {
    // ⚠️ BEFORE the base class, because on_ready() fills the document out of these.
    levels_ = scan_levels(app.game_dir());
    if (!only_.empty()) {
        // A round: its circuits, in the round's own order.
        std::vector<LevelEntry> kept;
        for (const std::string& name : only_)
            for (const LevelEntry& l : levels_)
                if (l.name == name) kept.push_back(l);
        levels_ = std::move(kept);
    }
    if (levels_.empty()) {
        std::fprintf(stderr, "No playable circuit under %s - nothing to show.\n",
                     app.game_dir().c_str());
        return false;
    }
    text_.load(app.game_dir(), config_language(app.game_dir()));
    scn_.load(app, "GLOBESCN", "THIRD3.RAW");

    // ⚠️ THE GLOBE IS BUILT BEFORE THE DOCUMENT, not after. `RmlUiScreen::enter` calls
    // `on_ready`, which registers the arrows and the stamp OUT OF the globe's own copy of
    // `GLOBESCN.SPR` - loading it afterwards would leave every widget blank on the first
    // frame and for ever after, since nothing registers them a second time.
    //
    // The globe and the background are this screen's own GL, released in exit(). Neither is
    // fatal: a copy of the game without `GLOBESCN.SCN` still gets a usable screen.
    if (!globe_.load(app.game_dir()))
        std::fprintf(stderr, "Globe: no sphere - the screen still works, it is just flat.\n");
    try {
        const Archive& scn = *game_archive(app.game_dir(), "GLOBESCN.SCN");
        if (auto it = scn.find("THIRD3.RAW"); it != scn.end()) background_.load(it->second);
    } catch (const std::exception&) {
        // No background: the canvas stays black behind the globe, which is not wrong.
    }
    return RmlUiScreen::enter(app);
}

void GlobeScreen::exit(App& app) {
    globe_.release();
    background_.release();
    RmlUiScreen::exit(app);
}

// Coming back from the presentation screen or from a race: the front end takes its own piece
// back. A no-op when it is already playing.
void GlobeScreen::resumed(App& app) {
    app.music().play_cd_track(kFrontEndMusicTrack);
    // Back from a split screen's series. Once a race of it has been run, choosing starts over;
    // back from the presentation screen before any, it takes back the last race chosen only -
    // Escape steps back one race at a time, there as here (on_back).
    if (app.locals().split() && only_.empty()) {
        Competition::LocalSeries& ser = app.competition().series();
        // QUIT SERIES, or OK after its last race: the series is over, choosing starts over.
        if (app.competition().leaving()) {
            ser.restart();
            app.competition().left();
        }
        if (ser.next > 0) ser.restart();
        else if (!ser.circuits.empty()) ser.circuits.pop_back();
        ser.next = 0;
        refresh(app);
    }
    // A championship race has been run: back to the championship, which takes the result.
    if (!only_.empty() && (app.competition().race_pending() || app.competition().leaving())) app.pop();
}

void GlobeScreen::on_ready(App& app) {
    register_widgets(app);
    if (document_) {
        static const struct { const char* id; const char* stem; } kSlots[] = {
            {"prev", "globe:left"}, {"next", "globe:right"}, {"ok", "globe:stamp"},
        };
        for (const auto& slot : kSlots)
            if (Rml::Element* box = document_->GetElementById(slot.id))
                box->SetInnerRML(Rml::String("<img src=\"") + slot.stem + "0\"/>");
    }
    // The stamp, not the left arrow: the arrows are on Left/Right of their own (see
    // handle_event), so the bottom pad button is free to mean OK - and must.
    set_focus_order({"prev", "next", "ok"}, "ok");
    refresh(app);
}

// The arrows and the passport stamp, out of `GLOBESCN.SPR` and through `THIRD3.RAW`'s palette.
// Registered once, under names the document then asks for like any other image.
void GlobeScreen::register_widgets(App& app) {
    static const struct { Globe::Widget widget; const char* stem; } kWidgets[] = {
        {Globe::Widget::LeftArrow, "globe:left"},
        {Globe::Widget::RightArrow, "globe:right"},
        {Globe::Widget::Stamp, "globe:stamp"},
    };
    for (const auto& w : kWidgets) {
        for (int frame = 0; frame < 5; ++frame) {
            int iw = 0, ih = 0;
            std::vector<uint8_t> rgba = globe_.widget_image(w.widget, frame, iw, ih);
            if (rgba.empty()) continue;
            app.ui().renderer().register_image(std::string(w.stem) + std::to_string(frame),
                                               std::move(rgba), iw, ih);
        }
    }
}

// The three widgets are built ONCE, with an `<img>` inside each, and the animation then only
// swaps that image's `src`.
//
// ⚠️ NOT `SetInnerRML` EVERY FRAME, and not an `id` on the `<img>` either. Rebuilding the
// element ten times a second would throw away RmlUi's own hover state with it; and an id on
// the child would swallow clicks meant for the button, which is the trap
// enchainement-des-menus.md's rule 8 records - a click walks up to the nearest ancestor WITH
// an id, so a child that has one stops the walk one element short.
void GlobeScreen::refresh_widget_frames() {
    if (!document_) return;
    static const struct { const char* id; const char* stem; } kSlots[] = {
        {"prev", "globe:left"}, {"next", "globe:right"}, {"ok", "globe:stamp"},
    };
    for (const auto& slot : kSlots) {
        Rml::Element* box = document_->GetElementById(slot.id);
        if (!box || box->GetNumChildren() == 0) continue;
        box->GetChild(0)->SetAttribute("src", Rml::String(slot.stem) +
                                                  Rml::String(std::to_string(widget_frame_)));
    }
}

void GlobeScreen::refresh(App& app) {
    if (!document_) return;
    const auto set = [this](const char* id, const Rml::String& html) {
        if (Rml::Element* el = document_->GetElementById(id)) el->SetInnerRML(html);
    };
    // A split screen choosing several races: the original's own title for it, CHOOSE RACES.
    const Competition::LocalSeries& ser = app.competition().series();
    const bool picking = picking_series(app);
    set("title", scn_.text_rml(text_, picking ? "GLB_2P" : "GLB_SINGLE").c_str());

    const LevelEntry* e = current();
    if (picking && (e || !ser.circuits.empty())) {
        // The races already in, in the order they were chosen, then the one OK would add -
        // dimmed, since it is not in yet.
        set("chosen", text_.text("GLB_CHOSEN", "CHOSEN:").c_str());
        std::string list;
        size_t i = 0;
        for (const std::string& n : ser.circuits) {
            std::string title = n;
            for (const LevelEntry& l : levels_)
                if (l.name == n) title = l.title;
            list += "<div>" + std::to_string(++i) + ". " + title + "</div>";
        }
        if (e && ser.circuits.size() < static_cast<size_t>(ser.races))
            list += "<div class=\"next\">" + std::to_string(i + 1) + ". " + e->title + "</div>";
        set("chosen_name", list.c_str());
    }
    if (!e) {
        // Nothing chosen: the original writes nothing at all, and neither do we.
        set("place", "");
        set("blurb", "");
        if (!picking || ser.circuits.empty()) {
            set("chosen", "");
            set("chosen_name", "");
        }
    } else {
        if (!picking) {
            set("chosen", text_.text("GLB_CHOSEN", "CHOSEN:").c_str());
            set("chosen_name", e->title.c_str());
        }
        std::string blurb = e->description;
        if (picking && ser.circuits.size() < static_cast<size_t>(ser.races))
            blurb = "RACE " + std::to_string(ser.circuits.size() + 1) + " OF " +
                    std::to_string(ser.races) + "<br/>" + blurb;
        set("blurb", blurb.c_str());
        // The hand-written name, registered under its own index so RmlUi's texture cache does
        // not hand back the previous country's picture.
        if (e->name_index >= 0) {
            const std::string src = "globe:name" + std::to_string(e->name_index);
            std::vector<uint8_t> rgba = globe_.name_image(e->name_index);
            if (!rgba.empty()) {
                app.ui().renderer().register_image(src, std::move(rgba), Globe::kNameWidth,
                                                   Globe::kNameHeight);
                set("place", ("<img src=\"" + src + "\"/>").c_str());
            } else {
                set("place", "");
            }
        } else {
            set("place", "");
        }
    }

}

void GlobeScreen::choose(App& app, int delta) {
    const int n = static_cast<int>(levels_.size());
    if (n == 0) return;
    if (!chosen_) {
        // ⚠️ THE FIRST CHOICE IS THE FIRST CIRCUIT, whichever arrow was pressed - Kenya, which
        // is what the user's capture of the original shows. Stepping backwards from "nothing"
        // into the LAST circuit would be a different screen's behaviour, not this one's.
        chosen_ = true;
        level_ = 0;
    } else {
        level_ = (level_ + delta % n + n) % n;
    }
    const LevelEntry& e = levels_[static_cast<size_t>(level_)];
    if (e.on_globe()) {
        yaw_target_ = Globe::yaw_for(e.globe_x);
        pitch_target_ = Globe::pitch_for(e.globe_y);
        if (pitch_target_ > kMaxPitch) pitch_target_ = kMaxPitch;
        if (pitch_target_ < -kMaxPitch) pitch_target_ = -kMaxPitch;
    } else {
        // A whole planet, or a circuit `TOURN.TXT` never placed (CITY3, TEST1): there is
        // nowhere to swing to, so the globe simply keeps turning - which is also what tells
        // the player they are not looking at a country any more.
        pitch_target_ = 0.0f;
    }
    refresh(app);
}

void GlobeScreen::on_activate(App& app, const std::string& id) {
    if (id == "prev") { choose(app, -1); return; }
    if (id == "next") { choose(app, +1); return; }
    if (id == "ok") {
        // OK with nothing chosen picks the first circuit rather than doing nothing: a button
        // that looks pressable and is not is worse than a button that starts you off.
        if (!chosen_) { choose(app, +1); return; }
        const LevelEntry& e = levels_[static_cast<size_t>(level_)];
        // A split screen's series: each OK adds a race, until player one has them all.
        if (app.locals().split() && only_.empty()) {
            Competition::LocalSeries& ser = app.competition().series();
            if (ser.circuits.size() >= static_cast<size_t>(ser.races)) ser.restart();
            ser.circuits.push_back(e.name);
            ser.next = 0;
            if (ser.circuits.size() < static_cast<size_t>(ser.races)) {
                refresh(app);
                return;
            }
            for (const LevelEntry& l : levels_)
                if (l.name == ser.circuits.front()) {
                    app.push(std::make_unique<TrackScreen>(l));
                    return;
                }
        }
        app.push(std::make_unique<TrackScreen>(e));
        return;
    }
}

bool GlobeScreen::picking_series(App& app) const {
    return app.locals().split() && app.competition().series().races > 1 && only_.empty();
}

void GlobeScreen::on_back(App& app) {
    Competition::LocalSeries& ser = app.competition().series();
    if (picking_series(app) && !ser.circuits.empty()) {
        ser.circuits.pop_back();
        ser.next = 0;
        refresh(app);
        return;
    }
    app.pop();
}

bool GlobeScreen::handle_event(App& app, const SDL_Event& e) {
    // ⚠️ TAKEN BEFORE THE BASE CLASS, and only these: left/right turn the globe here instead of
    // walking the focus. See the header for why that is worth the one exception.
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        if (e.key.keysym.sym == SDLK_LEFT) { choose(app, -1); return true; }
        if (e.key.keysym.sym == SDLK_RIGHT) { choose(app, +1); return true; }
        const SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
            app.menu_click();
            on_activate(app, "ok");
            return true;
        }
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        const Uint8 b = e.cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) { choose(app, -1); return true; }
        if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { choose(app, +1); return true; }
        if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START) {
            app.menu_click();
            on_activate(app, "ok");
            return true;
        }
    }
    // The left stick sideways turns the globe as the d-pad does, edge-triggered the same way
    // RmlUiScreen treats it (one step per push, not one per frame held).
    if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
        static constexpr Sint16 kThreshold = 16000;
        const Sint16 v = e.caxis.value;
        if (v < -kThreshold && stick_armed_neg_) { choose(app, -1); stick_armed_neg_ = false; }
        if (v > kThreshold && stick_armed_pos_) { choose(app, +1); stick_armed_pos_ = false; }
        if (v > -kThreshold / 2) stick_armed_neg_ = true;
        if (v < kThreshold / 2) stick_armed_pos_ = true;
        return true;
    }
    return RmlUiScreen::handle_event(app, e);
}

void GlobeScreen::update(App& app, double dt_ms) {
    const LevelEntry* e = current();
    if (!e || !e->on_globe()) {
        // Nothing chosen, a planet, or a circuit with no place: it turns on itself.
        yaw_ += kIdleTurnsPerSecond * static_cast<float>(dt_ms) / 1000.0f;
        yaw_ -= std::floor(yaw_);
        pitch_ += shortest_delta(pitch_, pitch_target_) *
                  std::min(1.0f, kEasePerMs * static_cast<float>(dt_ms));
    } else {
        const float t = std::min(1.0f, kEasePerMs * static_cast<float>(dt_ms));
        yaw_ += shortest_delta(yaw_, yaw_target_) * t;
        pitch_ += shortest_delta(pitch_, pitch_target_) * t;
    }

    // The arrows and the stamp wibble - the original animates them with `anim 5,5,...` in
    // GLOBESCN.TXT, five frames apiece. ⚠️ A CLOCK, NEVER A FRAME COUNT: the player may be at
    // 240 fps.
    anim_ms_ += dt_ms;
    const int frame = static_cast<int>(anim_ms_ / 110.0) % 5;
    if (frame != widget_frame_) {
        widget_frame_ = frame;
        refresh_widget_frames();
    }

    RmlUiScreen::update(app, dt_ms);
}

void GlobeScreen::draw(App& app) {
    // ⚠️ ORDER MATTERS AND THE DOCUMENT CARRIES NO BACKGROUND OF ITS OWN: the globe has to land
    // between the artwork and the text, and an `<img id="bg">` inside the document would be
    // drawn by RmlUi AFTER this, hiding the sphere. So the `.RAW` is drawn here by hand.
    background_.draw();
    if (canvas_w_ > 0 && canvas_h_ > 0) {
        const LevelEntry* e = current();
        // Drawn at the rung's resolution, like every 3D model in a menu: a square around the
        // disc, with room for the chosen circuit's diamond on the rim.
        // ⚠️ EACH BODY HAS ITS OWN SIZE (user, 2026-09-28): the original globe's radius is 0x200
        // for the Earth, 0x155 for Mars and Venus, 0x100 for the Moon - chosen by the body on show
        // (0 Earth, 1 Mars, 2 Venus, 3 Moon).
        const GlobeBody body = e ? body_of(*e) : GlobeBody::Earth;
        const float body_scale = body == GlobeBody::Earth ? 1.0f
                                 : body == GlobeBody::Moon ? 256.0f / 512.0f
                                                           : 341.0f / 512.0f;
        const float r = kGlobeRadius * static_cast<float>(canvas_h_) * body_scale;
        const float half = r * 1.1f;
        const float gx = kGlobeCentreX * static_cast<float>(canvas_w_);
        const float gy = kGlobeCentreY * static_cast<float>(canvas_h_);
        const std::vector<GlobeMarker> marks = [&] {
            std::vector<GlobeMarker> marks;
            // Only the Earth carries markers: Mars, Venus and the Moon ARE the
            // destination, which is what `-1,-1,<n>` means (formats/tourn.h).
            if (e && e->planet != 0) return marks;
            // ⚠️ ONLY THE CHOSEN CIRCUIT IS PINNED. The original plants one
            // marker, not twenty-one: a globe peppered with red diamonds tells the
            // player nothing they can act on, and buries the country they are
            // looking at under the other twenty. See show_all_markers().
            for (size_t i = 0; i < levels_.size(); ++i) {
                const LevelEntry& l = levels_[i];
                if (!l.on_globe()) continue;
                const bool selected = chosen_ && static_cast<int>(i) == level_;
                if (!selected && !show_all_markers()) continue;
                marks.push_back({l.globe_x, l.globe_y, selected});
            }
            return marks;
        }();
        app.presentation().draw_model(
            gx - half, gy - half, 2.0f * half, 2.0f * half,
            [&](int tw, int th, float x, float y, float w, float h) {
                globe_.draw(tw, th, x + w * 0.5f, y + h * 0.5f, r * (w / (2.0f * half)), yaw_,
                            pitch_, body, marks);
            });
    }
    RmlUiScreen::draw(app);
}

} // namespace bigred
