#include "results_screen.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "app/app.h"
#include "game/competition.h"
#include "formats/level_script.h"   // kGameTickMs

namespace bigred {
namespace {

constexpr const char* kStyle = R"(
/* Under RES_RESULT, which wibbles at the top as in the original (`@X4,2`). Everything here
   is read - the place, the times, the standings - so it is steady lettering. */
#panel { position: absolute; left: 8%; top: 17%; width: 84%; }
#race { font-size: 16dp; color: #c0c0c0; margin-bottom: 24dp; }
#place { font-size: 32dp; color: #ffdf40; margin-bottom: 8dp; white-space: nowrap; }
#total { font-size: 24dp; margin-bottom: 24dp; white-space: nowrap; }
/* Two columns, the laps and the standings side by side: a network race has up to twelve
   names, and stacked under the laps they ran off the bottom of the screen. */
.column { display: inline-block; vertical-align: top; width: 44%; margin-right: 4%; }
.head { font-size: 16dp; color: #ffdf40; margin-bottom: 8dp; }
.lap-row { font-size: 16dp; color: #c0c0c0; margin-bottom: 4dp; white-space: nowrap; }
#heading { font-size: 16dp; color: #ffdf40; margin-bottom: 8dp; }
/* The standings take the wider column: place, name, time, and a series' points. */
#laps { width: 30%; }
#table { width: 64%; margin-right: 0; }
.row { font-size: 14dp; color: #c0c0c0; margin-bottom: 3dp; white-space: nowrap; }
.row.local { color: #ffffff; }
.cell { display: inline-block; }
.c-place { width: 8%; }
.c-name { width: 44%; }
.c-time { width: 24%; }
.c-pts { width: 24%; }
/* After the race: our own buttons, steady lettering on a dark plate as on the championship
   screen, along the bottom left of RESULSCN's OK. */
#after { position: absolute; left: 4%; bottom: 5%; width: 72%; }
.btn {
    display: inline-block; font-size: 18dp; color: #ffffff; background: #00000090;
    padding: 6dp 12dp; margin-right: 12dp; white-space: nowrap;
}
.btn:hover, .btn:focus { color: #ffdf40; }
)";

// "M:SS.CC" out of a tick count in the race's own 14ms units (sim/race.h).
std::string format_ticks(uint32_t ticks) {
    const uint32_t ms = ticks * static_cast<uint32_t>(kGameTickMs);
    const uint32_t minutes = ms / 60000;
    const uint32_t seconds = (ms / 1000) % 60;
    const uint32_t hundredths = (ms % 1000) / 10;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u:%02u.%02u", minutes, seconds, hundredths);
    return buf;
}

std::string ordinal(int place) {
    const char* suffix = "TH";
    if (place % 100 < 11 || place % 100 > 13) {
        switch (place % 10) {
        case 1: suffix = "ST"; break;
        case 2: suffix = "ND"; break;
        case 3: suffix = "RD"; break;
        default: break;
        }
    }
    return std::to_string(place) + suffix;
}

} // namespace

bool ResultsScreen::enter(App& app) {
    scn_.load(app, "RESULSCN");
    text_.load(app.game_dir(), config_language(app.game_dir()));
    std::string body = scn_.document_open(kStyle) + scn_.text_rml(text_, "RES_RESULT") +
        "<div id=\"panel\">\n"
        "<div id=\"race\">RACE: " + setup_.level_title + "</div>\n"
        "<div id=\"place\">" + ordinal(setup_.place) + " PLACE</div>\n"
        "<div id=\"total\">TOTAL TIME: " + format_ticks(setup_.finish_ticks) + "</div>\n";
    if (!setup_.heading.empty()) body += "<div id=\"heading\">" + setup_.heading + "</div>\n";
    body += "<div class=\"column\" id=\"laps\"><div class=\"head\">YOUR LAPS</div>\n";
    for (size_t i = 0; i < setup_.lap_ticks.size(); ++i) {
        body += "<div class=\"lap-row\">LAP " + std::to_string(i + 1) + ": " +
                format_ticks(setup_.lap_ticks[i]) + "</div>\n";
    }
    body += "</div>\n";
    if (!setup_.rows.empty()) {
        std::vector<Setup::Row> rows = setup_.rows;
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Setup::Row& a, const Setup::Row& b) { return a.place < b.place; });
        const bool series = std::any_of(rows.begin(), rows.end(),
                                        [](const Setup::Row& r) { return r.total_points >= 0; });
        body += "<div class=\"column\" id=\"table\"><div class=\"head\">STANDINGS</div>\n";
        for (const Setup::Row& r : rows) {
            using S = Setup::Row::Status;
            const std::string time = r.status == S::Finished ? format_ticks(r.finish_ticks)
                                     : r.status == S::Wrecked ? std::string("WRECKED")
                                     : r.status == S::Left    ? std::string("LEFT")
                                                              : std::string("-");
            std::string pts;
            if (r.total_points >= 0)
                pts = "+" + std::to_string(std::max(0, r.race_points)) + "  " +
                      std::to_string(r.total_points) + " PTS";
            body += std::string("<div class=\"row") + (r.local ? " local" : "") + "\">" +
                    "<span class=\"cell c-place\">" + std::to_string(r.place) + ".</span>" +
                    "<span class=\"cell c-name\">" + r.name + "</span>" +
                    "<span class=\"cell c-time\">" + time + "</span>" +
                    (series ? "<span class=\"cell c-pts\">" + pts + "</span>" : std::string()) +
                    "</div>\n";
        }
        body += "</div>\n";
    }
    body += "</div>\n";
    // Our own two, along the bottom, left of the OK.
    if (setup_.replay || !setup_.quit_label.empty()) {
        body += "<div id=\"after\">";
        if (setup_.replay) body += "<div class=\"btn\" id=\"again\">RACE AGAIN</div>";
        if (!setup_.quit_label.empty())
            body += "<div class=\"btn\" id=\"quit\">" + setup_.quit_label + "</div>";
        body += "</div>\n";
    }
    // RESULSCN's OK: `anisprite 1, 5,1,260,155` in `mbox 1, 260,155,55,45`.
    body += ScnScreen::box_open("continue", 260, 155, 55, 45) +
            scn_.anisprite_rml(5, 5, 260, 155, 260, 155) + "</div>\n</body></rml>";
    doc_source_ = std::move(body);
    return RmlUiScreen::enter(app);
}

void ResultsScreen::on_ready(App&) {
    std::vector<std::string> focus;
    if (setup_.replay) focus.push_back("again");
    if (!setup_.quit_label.empty()) focus.push_back("quit");
    focus.push_back("continue");
    set_focus_order(focus, "continue");
}

void ResultsScreen::update(App& app, double dt_ms) {
    shown_ms_ += dt_ms;
    if (setup_.auto_continue_ms > 0.0 && shown_ms_ >= setup_.auto_continue_ms && !left_) {
        on_activate(app, setup_.auto_replay ? "again" : "continue");
        return;
    }
    RmlUiScreen::update(app, dt_ms);
}

void ResultsScreen::on_activate(App& app, const std::string& id) {
    if (left_) return;
    if (id == "continue") {
        if (setup_.ok_leaves) app.competition().leave();
        leave(app);
    } else if (id == "quit") {
        app.competition().leave();
        leave(app);
    } else if (id == "again" && setup_.replay) {
        // The same race, pushed where this screen and the finished race were: the screen that
        // started it (the presentation) is never resumed in between, so it still knows a race
        // of its is running - a series moves on after the replay, not before.
        std::printf("[results] RACE AGAIN\n");
        std::fflush(stdout);
        app.competition().replay_race();
        left_ = true;
        app.pop();
        app.pop();
        app.push(setup_.replay());
    }
}

// Escape is OK, never QUIT: a championship is not thrown away by a key pressed once too often.
void ResultsScreen::on_back(App& app) { on_activate(app, "continue"); }

void ResultsScreen::leave(App& app) {
    left_ = true;
    // ⚠️ POPS TWICE: this screen AND the race underneath it, straight back to whatever pushed
    // the race (quick_race_screen.cpp today) - both applied in the same apply_pending() pass
    // (app/app.cpp), so the stack is never left holding the frozen, finished race. That screen's
    // OWN resumed() brings the front-end music back (quick_race_screen.cpp already does this
    // for the plain one-pop case); nothing extra to do here.
    app.pop();
    app.pop();
}

} // namespace bigred
