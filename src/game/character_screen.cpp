#include "character_screen.h"

#include <RmlUi/Core.h>

#include <cctype>
#include <cstdio>
#include <unordered_map>

#include "app/app.h"
#include "formats/game_files.h"
#include "formats/texture.h"   // kRawPaletteBytes, load_palette_rgb
#include "scene/character_model.h"

namespace bigred {
namespace {

// Where the turning driver stands, as fractions of the canvas. The original's `anisprite` and
// boxes leave the left half to the model and the text at `@X90`..; here the sheet takes the
// right half, so the driver keeps the left.
constexpr float kViewX = 0.0f, kViewY = 0.18f, kViewW = 0.28f, kViewH = 0.64f;

// Slower than the vehicle screen's turntable: a person is looked at, not inspected.
constexpr float kTurnsPerSecond = 0.07f;

// CHARSCN.TXT's own screen: CHAR_HED wibbling at `@X30,10`, the COLOUR box (220,45) and the
// PERSON box (220,100) on the right with their wibbly `@C8` labels, the animated OK at
// 250,155. The driver turns on the left; the fact sheet is where the original writes it,
// `@X90,40`, between the two - steady lettering, because it is read (the user asked for the
// biographies to stay, 2026-09-26).
constexpr const char* kStyle = R"(
#sheet { position: absolute; left: 28.5%; top: 20%; width: 39.5%; }
#sheet .label { font-size: 16dp; color: #a0a0c0; }
#sheet .value { font-size: 16dp; color: #ffffff; }
#sheet .gap { height: 8dp; }
/* The chosen colour's name, under its box, in the colour itself. */
#colour_name { position: absolute; left: 69.5%; top: 44%; width: 30%; font-size: 16dp; white-space: nowrap; }
)";

// The game's font has no lowercase, and RML is XML.
std::string rml_upper(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string hex(uint8_t r, uint8_t g, uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    return buf;
}

} // namespace

const char* CharacterScreen::document_source() const { return doc_.c_str(); }

bool CharacterScreen::enter(App& app) {
    const PlayerProfile& p = target_ ? *target_ : app.profile();
    character_ = ((p.character % kCharacterCount) + kCharacterCount) % kCharacterCount;
    colour_ = p.character_color;
    text_.load(app.game_dir(), config_language(app.game_dir()));
    try {
        const Archive& scn = *game_archive(app.game_dir(), "CHARSCN.SCN");
        if (auto it = scn.find("DRIVER.RAW");
            it != scn.end() && it->second.size() >= kRawPaletteBytes) {
            palette_ = load_palette_rgb(it->second.data(), kRawPaletteBytes);
            background_.load(it->second);
        }
    } catch (const std::exception&) {
        // No picture and no palette: the screen still names the six, which is its purpose.
    }
    scn_.load(app, "CHARSCN", "DRIVER.RAW");
    // No <img id="bg">: the picture is drawn by hand UNDER the turning driver (see draw()).
    doc_ = "<rml><head><style>" + scn_.focus_css() + kStyle + "</style></head><body>" +
           scn_.text_rml(text_, "CHAR_HED") +
           "<div id=\"sheet\" class=\"wrap-ok\"></div><div id=\"colour_name\"></div>" +
           ScnScreen::box_open("colour", 220, 45, 95, 32) +
           scn_.text_rml(text_, "CHAR_COLOR", 220, 45, "b") + "</div>" +
           ScnScreen::box_open("next", 220, 100, 95, 32) +
           scn_.text_rml(text_, "CHAR_NXT", 220, 100) + "</div>" +
           ScnScreen::box_open("ok", 250, 155, 65, 45) +
           scn_.anisprite_rml(0, 5, 250, 155, 250, 155) + "</div></body></rml>";
    return RmlUiScreen::enter(app);
}

void CharacterScreen::exit(App& app) {
    display_.release();
    background_.release();
    RmlUiScreen::exit(app);
}

void CharacterScreen::on_ready(App& app) {
    set_focus_order({"colour", "next", "ok"}, "ok");
    refresh(app);
}

void CharacterScreen::refresh(App& app) {
    const std::vector<ColorPreset>& presets = color_presets();
    const ColorPreset* preset =
        presets.empty() ? nullptr : &presets[static_cast<size_t>(colour_) % presets.size()];
    const VehicleTint tint = preset ? VehicleTint{preset->r, preset->g, preset->b} : VehicleTint{};
    display_.load_character(app.game_dir(), character_, palette_, preset ? &tint : nullptr);
    if (!document_) return;

    const CharacterInfo& info = character_info(character_);
    // The fact sheet: a piece ending in ':' is a label, an empty one a blank line, anything
    // else a value.
    const std::vector<std::string> lines = text_.broken_lines(info.bio_key);
    std::string sheet;
    for (const std::string& line : lines) {
        if (line.empty()) {
            sheet += "<div class=\"gap\"></div>";
            continue;
        }
        const bool label = line.back() == ':';
        sheet += std::string("<div class=\"") + (label ? "label" : "value") + "\">" +
                 rml_upper(line) + "</div>";
    }
    if (sheet.empty()) sheet = "<div class=\"value\">" + std::string(info.first_name) + "</div>";
    if (Rml::Element* e = document_->GetElementById("sheet")) e->SetInnerRML(sheet);

    // Under the COLOUR box: the preset's name, painted in the preset itself.
    if (Rml::Element* e = document_->GetElementById("colour_name"))
        e->SetInnerRML(preset ? "<span style=\"color: " + hex(preset->r, preset->g, preset->b) +
                                    ";\">" + rml_upper(preset->name) + "</span>"
                              : std::string());
}

void CharacterScreen::update(App& app, double dt_ms) {
    turn_ += kTurnsPerSecond * static_cast<float>(dt_ms) / 1000.0f;
    if (turn_ >= 1.0f) turn_ -= 1.0f;
    RmlUiScreen::update(app, dt_ms);
}

void CharacterScreen::draw(App& app) {
    // The picture, then the driver, then the document - RmlUi paints last, see car_screen.
    background_.draw();
    if (canvas_w_ > 0 && canvas_h_ > 0) {
        const float cw = static_cast<float>(canvas_w_), ch = static_cast<float>(canvas_h_);
        app.presentation().draw_model(
            kViewX * cw, kViewY * ch, kViewW * cw, kViewH * ch,
            [&](int tw, int th, float x, float y, float w, float h) {
                display_.draw(tw, th, x, y, w, h, turn_);
            });
    }
    RmlUiScreen::draw(app);
}

void CharacterScreen::on_activate(App& app, const std::string& id) {
    if (id == "prev" || id == "next") {
        character_ = (character_ + (id == "next" ? 1 : kCharacterCount - 1)) % kCharacterCount;
        refresh(app);
        return;
    }
    if (id == "colour") {
        // Cyclic, like the original's `col` box.
        const int n = static_cast<int>(color_presets().size());
        if (n > 0) colour_ = (colour_ + 1) % n;
        refresh(app);
        return;
    }
    if (id == "ok") {
        PlayerProfile& p = target_ ? *target_ : app.profile();
        p.character = character_;
        p.character_color = colour_;
        app.pop();
    }
}

} // namespace bigred
