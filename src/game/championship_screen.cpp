#include "championship_screen.h"

#include <RmlUi/Core.h>

#include <cctype>

#include <cstdio>
#include <memory>
#include <random>

#include "app/app.h"
#include "formats/tourn.h"   // kFrontEndMusicTrack
#include "game/globe_screen.h"
#include "game/track_screen.h"

namespace bigred {
namespace {

// The round and its circuits are READ - a list to choose from - so they stay in the steady
// lettering, under a wibbly title; BACK is a wibbly row at the bottom, as on NETSCN.
constexpr const char* kStyle = R"(
#panel { position: absolute; left: 8%; top: 19%; width: 84%; }
.line { font-size: 16dp; margin-bottom: 8dp; }
.dim { color: #a0a0c0; }
.news { font-size: 16dp; color: #a0ffa0; margin-top: 8dp; margin-bottom: 16dp; }
.news.bad { color: #ff9090; }
.entry {
    display: block; font-size: 24dp; color: #ffffff; background: #00000080;
    padding: 8dp 16dp; margin-bottom: 12dp; width: 80%; white-space: nowrap;
}
.entry:hover, .entry:focus { color: #ffdf40; }
)";

std::string ordinal(int place) {
    const char* suffix = (place == 1) ? "ST" : (place == 2) ? "ND" : (place == 3) ? "RD" : "TH";
    return std::to_string(place) + suffix;
}

} // namespace

const char* ChampionshipScreen::document_source() const { return doc_.c_str(); }

bool ChampionshipScreen::enter(App& app) {
    levels_ = scan_levels(app.game_dir());
    rounds_ = load_tourn_catalog(app.game_dir()).rounds;
    comp_ = &app.competition();
    new_championship();
    scn_.load(app, "CHOOSCN", "CONFIG4B.RAW");
    text_.load(app.game_dir(), config_language(app.game_dir()));
    std::string back = text_.text("CHOOSE_BACK", "BACK");
    for (char& c : back) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    doc_ = scn_.document_open(kStyle) + scn_.words_rml("CHAMPIONSHIP", 30, 3) +
           "<div id=\"panel\"><div id=\"body\"></div></div>" +
           ScnScreen::box_open("back", 10, 165, 310, 24) + scn_.words_rml(back, 48, 165, -1, 10, 165) +
           "</div></body></rml>";
    return RmlUiScreen::enter(app);
}

void ChampionshipScreen::exit(App& app) {
    if (comp_) comp_->end();
    RmlUiScreen::exit(app);
}

void ChampionshipScreen::new_championship() {
    std::vector<std::pair<std::string, int>> playable;
    for (const LevelEntry& l : levels_) playable.emplace_back(l.name, l.difficulty);
    Championship c;
    if (random_) {
        std::random_device rd;
        c.start_random(rounds_, playable, rd());
    } else {
        std::vector<std::string> names;
        for (const auto& p : playable) names.push_back(p.first);
        c.start(rounds_, names);
    }
    comp_->start_championship(std::move(c));
    message_.clear();
}

const LevelEntry* ChampionshipScreen::entry(const std::string& level) const {
    for (const LevelEntry& l : levels_)
        if (l.name == level) return &l;
    return nullptr;
}

void ChampionshipScreen::on_ready(App&) { rebuild(); }

void ChampionshipScreen::resumed(App& app) {
    app.music().play_cd_track(kFrontEndMusicTrack);
    // The race just run, recorded by the race itself (Competition::race_over).
    Competition::Taken t;
    if (comp_->take_race(t)) {
        const LevelEntry* e = entry(t.level);
        const std::string where = e ? e->title : t.level;
        switch (t.outcome) {
        case Championship::Outcome::Continue:
            message_ = (t.place == 1 ? "YOU WON " : ordinal(t.place) + " IN ") + where;
            break;
        case Championship::Outcome::NextRound:
            message_ = "YOU WON " + where + " - ROUND " + std::to_string(t.round_before + 1) +
                       " COMPLETE";
            break;
        case Championship::Outcome::Champion:
            message_ = "YOU ARE THE CHAMPION";
            break;
        case Championship::Outcome::Eliminated:
            message_ = ordinal(t.place) + " IN " + where + " - NOT ENOUGH WINS LEFT";
            break;
        }
    }
    // QUIT CHAMPIONSHIP on the results: it ends here, as BACK would (exit() ends it).
    if (comp_->leaving()) {
        app.pop();
        return;
    }
    rebuild();
}

void ChampionshipScreen::rebuild() {
    if (!document_) return;
    Rml::Element* body = document_->GetElementById("body");
    if (!body) return;
    std::string html;
    std::vector<std::string> focus;
    if (!champ().valid()) {
        html += "<div class=\"line dim\">TOURN.TXT NOT FOUND - NO CHAMPIONSHIP</div>";
    } else {
        const std::string round = champ().final_round()
                                      ? std::string("THE FINAL")
                                      : "ROUND " + std::to_string(champ().round() + 1) + " OF " +
                                            std::to_string(champ().rounds() - 1);
        html += "<div class=\"line\">" + round + "</div>";
        html += "<div class=\"line dim\">WINS: " + std::to_string(champ().wins()) +
                " OF " + std::to_string(champ().need()) + " NEEDED - RACES LEFT: " +
                std::to_string(champ().round_size() - champ().raced()) + "</div>";
        if (!message_.empty()) {
            const bool bad = champ().over() && !champ().won();
            html += std::string("<div class=\"news wrap-ok") + (bad ? " bad" : "") + "\">" + message_ +
                    "</div>";
        }
        if (!champ().over()) {
            // The original's way: the round's circuits on the globe (GLOBESCN, ROUND mode).
            html += "<div class=\"entry\" id=\"globe\">CHOOSE ON THE GLOBE</div>";
            focus.push_back("globe");
            static const char* kDifficulty[3] = {"EASY", "MEDIUM", "HARD"};
            for (const std::string& l : champ().available()) {
                const LevelEntry* e = entry(l);
                std::string label = e ? e->title : l;
                if (e && e->difficulty >= 0 && e->difficulty <= 2)
                    label += std::string(" - ") + kDifficulty[e->difficulty];
                html += "<div class=\"entry\" id=\"lvl_" + l + "\">" + label + "</div>";
                focus.push_back("lvl_" + l);
            }
            // Before the first race only: which championship this is.
            if (champ().round() == 0 && champ().raced() == 0) {
                html += std::string("<div class=\"entry\" id=\"mode\">ROUNDS: ") +
                        (random_ ? "DRAWN AT RANDOM" : "AS THE ORIGINAL") + "</div>";
                focus.push_back("mode");
            }
        } else {
            html += "<div class=\"entry\" id=\"again\">NEW CHAMPIONSHIP</div>";
            focus.push_back("again");
        }
    }
    focus.push_back("back");   // the wibbly row under the panel, built once in enter()
    body->SetInnerRML(html.c_str());
    set_focus_order(focus, focus.front());
}

void ChampionshipScreen::on_activate(App& app, const std::string& id) {
    if (id == "back") {
        app.pop();
        return;
    }
    if (id == "again") {
        new_championship();
        rebuild();
        return;
    }
    if (id == "mode") {
        random_ = !random_;
        new_championship();
        rebuild();
        return;
    }
    if (id == "globe") {
        app.push(std::make_unique<GlobeScreen>(champ().available()));
        return;
    }
    if (id.rfind("lvl_", 0) == 0) {
        const LevelEntry* e = entry(id.substr(4));
        if (!e) return;
        app.push(std::make_unique<TrackScreen>(*e));
    }
}

} // namespace bigred
