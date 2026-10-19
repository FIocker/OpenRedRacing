#include "menu_cursor.h"

#include <random>
#include <unordered_map>

#include "app/player_profile.h"   // color_presets
#include "formats/game_files.h"
#include "formats/sprite.h"
#include "formats/threedw.h"      // load_palette_rgb

namespace bigred {
namespace {
constexpr int kFrames = 8;
// Every 5 ticks of 14 ms, like every other animated widget of the menus.
constexpr uint32_t kFrameMs = 70;
}

bool MenuCursor::load(const std::string& game_dir) {
    const Archive& always = *game_archive_or_empty(game_dir, "ALWAYS.WAP");
    auto spr = always.find("ALWAYS.SPR");
    auto col = always.find("DEFAULT.COL");
    if (spr == always.end() || col == always.end()) return false;
    const std::vector<SpriteGlyph> glyphs = decode_spr(spr->second.data(), spr->second.size());
    const std::vector<RGB> palette = load_palette_rgb(col->second.data(), col->second.size());
    if (glyphs.size() < kFrames || palette.size() < 256) return false;

    // The colour of this launch: one of the presets, the bright one on 48 and three fifths of it
    // on 49 - DEFAULT.COL's own pair is fc0000 / a00000, the same ratio.
    const std::vector<ColorPreset>& presets = color_presets();
    RGB bright = palette[48], dark = palette[49];
    if (!presets.empty()) {
        std::random_device rd;
        const ColorPreset& p = presets[rd() % presets.size()];
        bright = RGB{p.r, p.g, p.b};
        dark = RGB{static_cast<uint8_t>(p.r * 3 / 5), static_cast<uint8_t>(p.g * 3 / 5),
                   static_cast<uint8_t>(p.b * 3 / 5)};
    }

    frames_.clear();
    hot_x_ = hot_y_ = -1;
    for (int i = 0; i < kFrames; ++i) {
        const SpriteGlyph& g = glyphs[static_cast<size_t>(i)];
        if (!g.valid()) return false;
        Frame f;
        f.w = g.width;
        f.h = g.height;
        f.rgba.assign(static_cast<size_t>(g.width) * g.height * 4, 0);
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x) {
                const uint8_t idx = g.pixels[static_cast<size_t>(y) * g.width + x];
                if (idx == 0) continue;
                // The arrow's tip is its first pixel from the top-left of the first frame.
                if (i == 0 && hot_x_ < 0) { hot_x_ = x; hot_y_ = y; }
                const RGB c = idx == 48 ? bright : (idx == 49 ? dark : palette[idx]);
                const size_t o = (static_cast<size_t>(y) * g.width + x) * 4;
                f.rgba[o] = c.r;
                f.rgba[o + 1] = c.g;
                f.rgba[o + 2] = c.b;
                f.rgba[o + 3] = 255;
            }
        frames_.push_back(std::move(f));
    }
    if (hot_x_ < 0) hot_x_ = hot_y_ = 0;
    return true;
}

void MenuCursor::build(int scale) {
    for (SDL_Cursor* c : cursors_) SDL_FreeCursor(c);
    cursors_.clear();
    scale_ = scale;
    shown_ = -1;
    for (const Frame& f : frames_) {
        const int w = f.w * scale, h = f.h * scale;
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!s) continue;
        auto* dst = static_cast<uint8_t*>(s->pixels);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = &f.rgba[(static_cast<size_t>(y / scale) * f.w + x / scale) * 4];
                uint8_t* q = dst + static_cast<size_t>(y) * s->pitch + static_cast<size_t>(x) * 4;
                q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
            }
        cursors_.push_back(SDL_CreateColorCursor(s, hot_x_ * scale, hot_y_ * scale));
        SDL_FreeSurface(s);
    }
}

void MenuCursor::update(bool menu, int window_h, uint32_t now_ms) {
    if (!menu) {
        // A race is driven with keys and pads: no pointer over it.
        if (!hidden_) { SDL_ShowCursor(SDL_DISABLE); hidden_ = true; }
        return;
    }
    if (hidden_) { SDL_ShowCursor(SDL_ENABLE); hidden_ = false; }
    if (frames_.empty()) return;
    int scale = window_h / 360;
    if (scale < 1) scale = 1;
    if (scale != scale_) build(scale);
    if (cursors_.size() != frames_.size()) return;
    const int f = static_cast<int>((now_ms / kFrameMs) % static_cast<uint32_t>(cursors_.size()));
    if (f != shown_ && cursors_[static_cast<size_t>(f)]) {
        SDL_SetCursor(cursors_[static_cast<size_t>(f)]);
        shown_ = f;
    }
}

void MenuCursor::release() {
    for (SDL_Cursor* c : cursors_) SDL_FreeCursor(c);
    cursors_.clear();
    frames_.clear();
    scale_ = 0;
    shown_ = -1;
}

} // namespace bigred
