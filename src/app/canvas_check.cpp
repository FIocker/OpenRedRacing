#include "canvas_check.h"

#include <SDL.h>

#include <cstdio>
#include <string>
#include <unordered_map>

#include "app/app.h"
#include "formats/game_files.h"
#include "formats/sprite.h"
#include "formats/threedw.h" // RGB, load_palette_rgb

namespace bigred {

bool CanvasCheck::enter(App& app) {
    ArchivePtr scn;
    try {
        scn = game_archive(app.game_dir(), scn_ + ".SCN");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Could not open %s.SCN: %s\n", scn_.c_str(), ex.what());
        return false;
    }

    // The screen's background is whatever `.RAW` it carries - the name is not always the
    // screen's own (LOGOSCN holds TEAM.RAW, GLOBESCN holds THIRD3.RAW), so take the first.
    for (const auto& [name, blob] : *scn) {
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".RAW") == 0) {
            if (background_.load(blob)) {
                std::printf("%s.SCN: background %s\n", scn_.c_str(), name.c_str());
                break;
            }
        }
    }
    if (!background_.ready())
        std::fprintf(stderr, "%s.SCN has no usable .RAW - the canvas will be empty.\n",
                     scn_.c_str());

    // The interface font, for the readout. Same two files the HUD uses.
    const Archive& always = *game_archive_or_empty(app.game_dir(), "ALWAYS.WAP");
    std::vector<SpriteGlyph> font;
    std::vector<RGB> palette;
    if (auto it = always.find("SML_ALPH.SPR"); it != always.end())
        font = decode_spr(it->second.data(), it->second.size());
    if (auto it = always.find("DEFAULT.COL"); it != always.end())
        palette = load_palette_rgb(it->second.data(), it->second.size());
    hud_.build({}, font, !font.empty(), palette);

    std::printf("Canvas check: the menus render HD, at the real resolution of a 4:3 box.\n"
                "The period look is a FILTER over the finished image, and it is OFF by\n"
                "default - it is the player's choice, not the engine's.\n"
                "  + / -  the resolution ladder: 320x200 / 640x480 / native\n"
                "  mouse  read canvas coordinates     ESC quit\n");
    return true;
}

void CanvasCheck::exit(App&) {
    background_.release();
}

bool CanvasCheck::handle_event(App& app, const SDL_Event& e) {
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        app.quit();
        return true;
    }
    if (e.type == SDL_MOUSEMOTION) {
        in_canvas_ = app.mouse_canvas(e.motion.x, e.motion.y, mx_, my_);
        return true;
    }
    return false;
}

void CanvasCheck::draw(App& app) {
    background_.draw();
    if (!hud_.ready()) return;

    const float w = static_cast<float>(app.presentation().canvas_width());
    const float h = static_cast<float>(app.presentation().canvas_height());
    if (w <= 0 || h <= 0) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    hud_.begin(static_cast<int>(w), static_cast<int>(h));

    // A frame on the canvas edge, one cell of the current rung thick.
    const float t = (app.presentation().lines() > 0)
                        ? h / static_cast<float>(app.presentation().lines())
                        : 2.0f;
    hud_.rect(0, 0, w, t, 1, 0, 0);
    hud_.rect(0, h - t, w, t, 1, 0, 0);
    hud_.rect(0, 0, t, h, 1, 0, 0);
    hud_.rect(w - t, 0, t, h, 1, 0, 0);

    // The L in the top-left corner: asymmetric on BOTH axes, so one glance says whether the
    // picture is flipped, mirrored or turned around - which reading the text cannot, since
    // upside-down text and mirrored text look alike at a distance. The long arm points DOWN.
    const float k = h / 200.0f;   // lay the marker out as if the canvas were 200 lines tall
    hud_.rect(6 * k, 24 * k, 4 * k, 26 * k, 0, 1, 0);
    hud_.rect(6 * k, 46 * k, 16 * k, 4 * k, 0, 1, 0);

    // The readout, and it is the POINT of this screen now: the same words drawn crisp and then
    // quantised. If the filter costs the text too much, it shows here first.
    const float fs = k * 1.4f;
    const float line = hud_.text_height(fs) + 4.0f * k;
    char s[128];
    std::snprintf(s, sizeof(s), "WINDOW %dX%d   CANVAS %dX%d", app.width(), app.height(),
                  static_cast<int>(w), static_cast<int>(h));
    hud_.text(s, 4 * k, 4 * k, fs, 1, 1, 1);
    const int grid = app.presentation().lines();
    if (grid > 0)
        std::snprintf(s, sizeof(s), "%s  -  %d LINES  -  + / - TO CHANGE",
                      app.presentation().mode_name(), grid);
    else
        std::snprintf(s, sizeof(s), "NATIVE  -  %dX%d  -  + / - TO CHANGE", app.width(),
                      app.height());
    hud_.text(s, 4 * k, 4 * k + line, fs, 1, 1, 0);
    if (in_canvas_)
        std::snprintf(s, sizeof(s), "POINTER %d,%d", static_cast<int>(mx_),
                      static_cast<int>(my_));
    else
        std::snprintf(s, sizeof(s), "POINTER OUTSIDE THE PICTURE");
    hud_.text(s, 4 * k, 4 * k + line * 2, fs, 1, 1, 1);
    // A line of ordinary prose, because that is what a menu is actually made of and short
    // uppercase labels flatter the filter.
    hud_.text("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789", 4 * k,
              h - 4 * k - hud_.text_height(fs), fs, 0.8f, 0.9f, 1.0f);

    hud_.flush();
    glDisable(GL_BLEND);
}

} // namespace bigred
