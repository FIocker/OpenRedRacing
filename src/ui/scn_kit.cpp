#include "scn_kit.h"

#include <cstdio>
#include <unordered_map>

#include "app/app.h"
#include "formats/game_files.h"
#include "formats/texture.h"   // kRawPaletteBytes
#include "ui/rml_render_interface.h"
#include "ui/rml_ui.h"

namespace bigred {
namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// RML needs its three specials escaped; the game's text has `&` in it ("RIG & TRUCK").
std::string escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out.push_back(c);
    }
    return out;
}

std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

} // namespace

bool ScnScreen::load(App& app, const std::string& name, const std::string& raw,
                     const std::string& spr, const std::string& raw_archive) {
    name_ = upper(name);
    const std::string raw_member = raw.empty() ? name_ + ".RAW" : upper(raw);
    const std::string spr_member = spr.empty() ? name_ + ".SPR" : upper(spr);
    ArchivePtr files_archive;
    try {
        files_archive = game_archive(app.game_dir(), name_ + ".SCN");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Menus: no %s.SCN (%s).\n", name_.c_str(), ex.what());
        return false;
    }
    const Archive& files = *files_archive;
    const std::string picture_archive = raw_archive.empty() ? name_ + ".SCN" : upper(raw_archive);
    bg_src_ = picture_archive + "/" + raw_member;
    const ArchivePtr pictures =
        raw_archive.empty() ? files_archive : game_archive_or_empty(app.game_dir(), picture_archive);
    if (auto it = pictures->find(raw_member);
        it != pictures->end() && it->second.size() >= kRawPaletteBytes)
        palette_ = load_palette_rgb(it->second.data(), kRawPaletteBytes);
    if (auto it = files.find(spr_member); it != files.end() && !palette_.empty()) {
        sprites_ = decode_spr(it->second.data(), it->second.size());
        for (size_t i = 0; i < sprites_.size(); ++i) {
            const SpriteGlyph& g = sprites_[i];
            if (!g.valid()) continue;
            std::vector<uint8_t> rgba(static_cast<size_t>(g.width) * g.height * 4, 0);
            for (size_t p = 0; p < g.pixels.size(); ++p) {
                const uint8_t idx = g.pixels[p];
                if (idx == 0) continue;   // palette index 0 is transparent, everywhere
                const RGB& c = palette_[idx];
                rgba[p * 4] = c.r;
                rgba[p * 4 + 1] = c.g;
                rgba[p * 4 + 2] = c.b;
                rgba[p * 4 + 3] = 255;
            }
            app.ui().renderer().register_image(sprite_src(static_cast<int>(i)), std::move(rgba),
                                               g.width, g.height);
        }
    }
    return true;
}

std::string ScnScreen::sprite_src(int i) const {
    if (i < 0 || i >= static_cast<int>(sprites_.size())) return {};
    return "scn:" + name_ + ":" + std::to_string(i);
}

int ScnScreen::sprite_w(int i) const {
    return (i >= 0 && i < static_cast<int>(sprites_.size())) ? sprites_[static_cast<size_t>(i)].width : 0;
}
int ScnScreen::sprite_h(int i) const {
    return (i >= 0 && i < static_cast<int>(sprites_.size())) ? sprites_[static_cast<size_t>(i)].height : 0;
}

std::string ScnScreen::ramp_colour(int ramp) const {
    // No `@C`: the lettering keeps the colour it is drawn in, index 16 - white in every menu
    // palette (CONFSCN's fce0f0, OPTSCN's fcfcfc), its outline index 47 staying dark.
    const size_t idx = ramp < 0 ? 16 : static_cast<size_t>(ramp & 15) * 16 + 2;
    RGB c{255, 255, 255};
    if (idx < palette_.size()) c = palette_[idx];
    char b[16];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
    return b;
}

std::string ScnScreen::text_rml(const TextTable& text, const std::string& key, int ox, int oy,
                                const std::string& extra) const {
    std::string out;
    bool open = false;
    int last_x = 0, last_y = 0;
    for (const TextTable::Run& r : text.layout(key)) {
        const std::string colour = ramp_colour(r.ramp);
        if (r.x == -2 && open) {
            out += "<span style=\"color:" + colour + ";\">" + escape(r.text) + "</span>";
            continue;
        }
        if (open) out += "</div>";
        // A run placed nowhere goes under the one before it, one wibbly line down.
        const int x = r.x >= 0 ? r.x : last_x;
        const int y = r.y >= 0 ? r.y : last_y + (r.x == -2 ? 0 : 30);
        last_x = x;
        last_y = y;
        out += "<div class=\"st" + (extra.empty() ? std::string() : " " + extra) +
               "\" style=\"left:" + num(scn_dp_x(x - ox)) + "dp;top:" + num(scn_dp_y(y - oy)) +
               "dp;color:" + colour + ";\">" + escape(r.text);
        open = true;
    }
    if (open) out += "</div>";
    return out;
}

std::string ScnScreen::words_rml(const std::string& words, int x, int y, int ramp, int ox,
                                 int oy, const std::string& extra) const {
    return "<div class=\"st" + (extra.empty() ? std::string() : " " + extra) +
           "\" style=\"left:" + num(scn_dp_x(x - ox)) + "dp;top:" + num(scn_dp_y(y - oy)) +
           "dp;color:" + ramp_colour(ramp) + ";\">" + escape(words) + "</div>";
}

std::string ScnScreen::box_open(const std::string& id, int x, int y, int w, int h,
                                const std::string& cls) {
    return "<div id=\"" + id + "\" class=\"" + cls + "\" style=\"position:absolute;left:" +
           num(scn_dp_x(x)) + "dp;top:" + num(scn_dp_y(y)) + "dp;width:" + num(scn_dp_x(w)) +
           "dp;height:" + num(scn_dp_y(h)) + "dp;\">";
}

std::string ScnScreen::sprite_rml(int i, int x, int y, int ox, int oy) const {
    const std::string src = sprite_src(i);
    if (src.empty()) return {};
    return "<img class=\"spr\" src=\"" + src + "\" style=\"left:" + num(scn_dp_x(x - ox)) +
           "dp;top:" + num(scn_dp_y(y - oy)) + "dp;width:" + num(scn_dp_x(sprite_w(i))) +
           "dp;height:" + num(scn_dp_y(sprite_h(i))) + "dp;\"/>";
}

std::string ScnScreen::anisprite_rml(int first, int count, int x, int y, int ox, int oy,
                                     const std::string& id) const {
    const std::string src = sprite_src(first);
    if (src.empty()) return {};
    // The frames are named scn:<NAME>:<i>; the base and the count tell RmlUiScreen which to
    // cycle through. Sized on the first frame: an anisprite's frames share their size.
    return "<img class=\"spr anim\"" + (id.empty() ? std::string() : " id=\"" + id + "\"") +
           " data-anim-base=\"scn:" + name_ + ":\" data-anim-first=\"" +
           std::to_string(first) + "\" data-anim-count=\"" + std::to_string(count) +
           "\" src=\"" + src + "\" style=\"left:" + num(scn_dp_x(x - ox)) + "dp;top:" +
           num(scn_dp_y(y - oy)) + "dp;width:" + num(scn_dp_x(sprite_w(first))) + "dp;height:" +
           num(scn_dp_y(sprite_h(first))) + "dp;\"/>";
}

std::string ScnScreen::focus_css() const {
    return ".hit:hover .st, .hit:focus .st { color: " + ramp_colour(kFocusRamp) + "; }"
           ".hit:hover .spr, .hit:focus .spr { transform: scale(1.15); }";
}

std::string ScnScreen::document_open(const std::string& css) const {
    return "<rml><head><style>" + focus_css() + css + "</style></head><body><img id=\"bg\" src=\"" +
           background_src() + "\"/>";
}

} // namespace bigred
