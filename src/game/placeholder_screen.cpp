#include "placeholder_screen.h"

#include <cctype>

#include "app/app.h"
#include "formats/localised_text.h"
#include "ui/scn_kit.h"

namespace bigred {

bool PlaceholderScreen::enter(App& app) {
    // The picture is named "SCREEN.SCN/PICTURE.RAW": its screen gives the wibbly lettering its
    // palette, as it would for any other rebuilt screen (ui/scn_kit.h).
    ScnScreen scn;
    const size_t dot = background_.find(".SCN/");
    if (dot != std::string::npos)
        scn.load(app, background_.substr(0, dot), background_.substr(dot + 5));
    TextTable text;
    text.load(app.game_dir(), config_language(app.game_dir()));
    std::string back = text.text("CHOOSE_BACK", "BACK");
    for (char& c : back) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    doc_source_ =
        scn.document_open("#subtitle { position: absolute; left: 10%; top: 45%; width: 80%;"
                          " text-align: center; font-size: 16dp; color: #c0c0c0; }") +
        scn.words_rml(title_, 30, 10) + "<div id=\"subtitle\">" + subtitle_ + "</div>" +
        ScnScreen::box_open("back", 10, 165, 310, 24) + scn.words_rml(back, 48, 165, -1, 10, 165) +
        "</div></body></rml>";
    return RmlUiScreen::enter(app);
}

void PlaceholderScreen::on_ready(App&) { set_focus_order({"back"}); }

void PlaceholderScreen::on_activate(App& app, const std::string& id) {
    if (id == "back") app.pop();
}

} // namespace bigred
