// A screen that says "not built yet" and goes back - honestly, not as a disguised half-feature.
//
// The rule: "un salon complet qui lance une course a un joueur est une etape honnete; un salon
// ecrit a moitie ne l'est pas". Championship
// and Multiplayer are exactly that half-written salon if faked here, so the main menu's five
// entries are ALL real and navigable today, and the two not yet built say so in plain text
// instead of silently doing nothing or crashing.
#pragma once

#include <string>

#include "app/rml_screen.h"

namespace bigred {

class PlaceholderScreen : public RmlUiScreen {
public:
    // `background`: an "ARCHIVE.SCN/MEMBER.RAW" pick, the closest original screen in spirit -
    // the user asked backgrounds to change per screen rather than reuse CONFSCN everywhere.
    // Defaults to the main menu's own when nothing closer exists.
    PlaceholderScreen(std::string title, std::string subtitle,
                       std::string background = "CONFSCN.SCN/CONFSCN.RAW")
        : title_(std::move(title)), subtitle_(std::move(subtitle)), background_(std::move(background)) {}

    bool enter(App& app) override;
    const char* name() const override { return "placeholder"; }

protected:
    const char* document_source() const override { return doc_source_.c_str(); }
    // Every placeholder shares one context: only one is ever on the stack at a time (the main
    // menu pushes at most one), unlike quick_race_screen which can sit under a race.
    const char* context_name() const override { return "placeholder"; }
    void on_ready(App& app) override;
    void on_activate(App& app, const std::string& id) override;

private:
    std::string title_, subtitle_, background_;
    std::string doc_source_;
};

} // namespace bigred
