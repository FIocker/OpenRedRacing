#include "race_board.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL_opengl.h>

#include "formats/game_files.h"
#include "formats/terrain.h"
#include "scene/level_scene.h"

namespace bigred {

void RaceBoard::build(const std::string& game_dir,
                      const std::unordered_map<std::string, Blob>& ingame,
                      const LevelScene& scene) {
    // The game's own artwork: the digits, the ordinals and the banners come from `GAMESVGA.SPR`
    // in INGAME.WAP, and free text from `SML_ALPH.SPR` in ALWAYS.WAP, both coloured by
    // `DEFAULT.COL`. A missing file is not fatal - the race runs without a board.
    //
    // ⚠️ `SML_ALPH.SPR`, NOT `MICRO.FNT`. The first HUD wrote with MICRO.FNT, which is what the
    // original uses for its own lap line - but it is five pixels tall and carries no outline,
    // and magnified to HUD size it is a smear (user, first race: "les nouvelles ecritures sont
    // completement illisibles"). SML_ALPH is the interface's own 8x8 alphabet, white on a black
    // outline. MICRO.FNT stays as the fallback, nothing more.
    const Archive& always = *game_archive_or_empty(game_dir, "ALWAYS.WAP");
    std::vector<SpriteGlyph> hud_sprites, hud_font;
    std::vector<RGB> hud_palette;
    bool font_indexed = false;
    auto spr = ingame.find("GAMESVGA.SPR");
    if (spr == ingame.end()) spr = ingame.find("GAMEVGA.SPR");
    if (spr != ingame.end()) hud_sprites = decode_spr(spr->second.data(), spr->second.size());
    if (auto alph = always.find("SML_ALPH.SPR"); alph != always.end()) {
        hud_font = decode_spr(alph->second.data(), alph->second.size());
        font_indexed = !hud_font.empty();
    }
    if (hud_font.empty()) {
        if (auto fnt = always.find("MICRO.FNT"); fnt != always.end())
            hud_font = decode_fnt(fnt->second.data(), fnt->second.size());
    }
    if (auto col = always.find("DEFAULT.COL"); col != always.end())
        hud_palette = load_palette_rgb(col->second.data(), col->second.size());
    // The MINI-MAP: the level's own `TOPMAP.SPR`, its 128x128 glyph, drawn through the level's
    // palette. Its projection is in its header.
    std::vector<SpriteGlyph> topmap;
    if (auto tm = scene.lev.find("TOPMAP.SPR"); tm != scene.lev.end())
        topmap = decode_spr(tm->second.data(), tm->second.size());
    const SpriteGlyph* map_glyph = (!topmap.empty() && topmap[0].valid()) ? &topmap[0] : nullptr;
    topmap_ = map_glyph ? topmap_projection(*map_glyph) : TopmapProjection();
    if (!hud_.build(hud_sprites, hud_font, font_indexed, hud_palette, map_glyph, &scene.palette))
        std::fprintf(stderr, "No HUD artwork: racing without a board.\n");
}

// --- the board --------------------------------------------------------------------------------
void RaceBoard::draw(const BoardFrame& f) {
    const int width = f.width, height = f.height;
    const Mat4& mvp = f.mvp;
    // Drawn last, over everything, and it reads ONLY from the race and the car - never from a
    // frame counter. The clock is the race's own tick count times 14 ms, so it advances at the
    // same rate at 30 fps and at 240.
    // ⚠️ AND NOT ON A FLY-BY: there is no lap, no clock and no damage to report, and a board
    // over a presentation shot is the one thing the original never does - the screen does not
    // ask for one there.
    if (!hud_.ready() || !f.session || !f.tuning || !f.nitros) return;
    namespace g = game_spr;
    const RacerProgress& me = f.session->race().progress(f.player);
    const VehicleSim& car = f.session->car(f.player);
    // Everything scales off the window height, so the board keeps its proportions when the
    // window is resized. The SVGA artwork is drawn for a 480-line screen.
    const float k = static_cast<float>(height) / 480.0f;
    const float margin = 16.0f * k;
    // SML_ALPH is 8 rows tall and is magnified by a WHOLE number (see Hud::text_scale), so this
    // is "about 1.4 screen pixels per glyph pixel" and lands on 3 at 1080p.
    const float fs = 1.4f * k;
    // The readouts are drawn in the game's own 42-pixel digits - the same set as the damage
    // gauge - so every number on the board is one typeface.
    const float ds = 0.36f * k;
    const float digit_h = hud_.sprite_height(g::kSmallDigit0) * ds;
    const float line = digit_h + 5.0f * k;
    // A label sits on the digits' own baseline rather than at their top.
    const float label_dy = (digit_h - hud_.text_height(fs)) * 0.5f;
    hud_.begin(width, height);

    // NAME TAGS over the other vehicles, in a network race: who is who (user, 2026-09-25).
    // A human carries the name of their profile, a ghost just "IA" - the user's word. Not in the
    // original, which had no names to show. Projected with the world's own matrix, so a tag
    // sits on its car wherever the camera is; it fades with distance and is not occluded by
    // the terrain, which at these sizes reads better than a tag blinking behind every bump.
    // The screen fills `tags` only in a network race or on a split screen.
    for (const BoardTag& t : f.tags) {
        const float v[4] = {t.x, t.y, t.z, 1.0f};
        float c[4];
        for (int row = 0; row < 4; ++row)
            c[row] = mvp.m[0][row] * v[0] + mvp.m[1][row] * v[1] + mvp.m[2][row] * v[2] +
                     mvp.m[3][row] * v[3];
        if (c[3] <= 1e-4f) continue;   // behind the camera
        const float sx = (c[0] / c[3] * 0.5f + 0.5f) * static_cast<float>(width);
        const float sy = (0.5f - c[1] / c[3] * 0.5f) * static_cast<float>(height);
        if (sx < -200.0f || sx > width + 200.0f || sy < -50.0f || sy > height + 50.0f) continue;
        // Full up to 6 000 world units, gone at 16 000 - about a third of a big circuit.
        const float d = t.distance;
        const float a = d < 6000.0f ? 1.0f : (d > 16000.0f ? 0.0f : (16000.0f - d) / 10000.0f);
        if (a <= 0.0f) continue;
        if (t.human)
            hud_.text_centred(t.text, sx, sy - hud_.text_height(fs), fs, 1.0f, 1.0f, 1.0f, a);
        else
            hud_.text_centred(t.text, sx, sy - hud_.text_height(fs), fs, 0.75f, 0.75f, 0.75f, a);
    }

    // Top RIGHT: the lap and the clock, right-aligned. The two top corners were swapped at the
    // user's request (2026-09-23) so the placing and the mini-map could share the left one.
    {
        const int shown = me.lap + 1 > f.session->race().laps() ? f.session->race().laps() : me.lap + 1;
        const float lap_w = hud_.text_width("LAP ", fs) + hud_.number_width(shown, ds) +
                            hud_.separator_width('/', ds) + hud_.number_width(f.session->race().laps(), ds);
        float pen = width - margin - lap_w;
        hud_.text("LAP", pen, margin + label_dy, fs, 1.0f, 1.0f, 1.0f);
        pen += hud_.text_width("LAP ", fs);
        pen = hud_.number(shown, pen, margin, ds);
        pen = hud_.separator('/', pen, margin, ds, 1.0f, 1.0f, 1.0f);
        hud_.number(f.session->race().laps(), pen, margin, ds);

        const uint32_t ms = f.session->race().race_ticks() * kGameTickMs;
        hud_.clock(ms, width - margin - hud_.clock_width(ds), margin + line, ds);
        // The last lap's time, once there is one.
        if (!me.lap_ticks.empty()) {
            const uint32_t lms = me.lap_ticks.back() * kGameTickMs;
            const float cw = hud_.clock_width(ds * 0.8f);
            const float lw = hud_.text_width("LAST ", fs);
            hud_.text("LAST", width - margin - cw - lw, margin + line * 2 + label_dy, fs, 0.7f,
                      0.8f, 1.0f);
            hud_.clock(lms, width - margin - cw, margin + line * 2, ds * 0.8f);
        }
    }

    // Top LEFT: the placing, as the game draws it - a large digit and its ordinal - and the
    // mini-map under it.
    float place_bottom = margin;
    {
        // Two digits past nine: a network race holds up to twelve (sim/race_session.h). The
        // original never needed it - six racers at most.
        const int place = me.place < 1 ? 1 : (me.place > 99 ? 99 : me.place);
        const int last = place % 10;
        const int ord = g::kOrdinalSt + ((place / 10) % 10 == 1 ? 3
                                         : last == 1          ? 0
                                         : last == 2          ? 1
                                         : last == 3          ? 2
                                                              : 3);
        const float pds = 0.9f * k;
        float pen = margin;
        if (place >= 10) {
            hud_.sprite(g::kDigit0 + place / 10, pen, margin, pds);
            pen += hud_.sprite_width(g::kDigit0 + place / 10) * pds;
        }
        hud_.sprite(g::kDigit0 + last, pen, margin, pds);
        pen += hud_.sprite_width(g::kDigit0 + last) * pds;
        hud_.sprite(ord, pen, margin, pds);
        place_bottom = margin + static_cast<float>(hud_.sprite_height(g::kDigit0 + last)) * pds;
    }

    // The mini-map. The original draws the 128-pixel map in a 640-wide screen, i.e. at the
    // SVGA artwork's own scale, which here is `k`, rounded to a whole number because it is pixel
    // art. Each racer is a coloured dot, centred on the pixel the original's projection gives -
    // see formats/sprite.h. ⚠️ NOT THE ORIGINAL'S COLOURING, which gave each racer its own dot
    // (GAMESVGA 14 + colour): at the user's request (2026-09-25, after a network test) the
    // player is RED (dot 0) and every opponent the same ORANGE (dot 4) - with up to twelve
    // racers and six colours, "which one is me" mattered more than "which one is who".
    if (hud_.has_minimap() && topmap_.valid() && f.show_minimap) {
        const float ms = Hud::text_scale(k);
        const float mx = margin, my = place_bottom + 6.0f * k;
        hud_.minimap(mx, my, ms, 0.85f);
        // Every racer's dot, the player's LAST so it is never hidden under an opponent's.
        auto dot = [&](const VehicleSim& c, int glyph) {
            int px = 0, py = 0;
            topmap_pixel(topmap_, c.world_x(), c.world_z(), px, py);
            const float dot_w = static_cast<float>(hud_.sprite_width(glyph)) * ms;
            const float dot_h = static_cast<float>(hud_.sprite_height(glyph)) * ms;
            hud_.sprite(glyph, mx + px * ms - dot_w * 0.5f, my + py * ms - dot_h * 0.5f, ms);
        };
        // The colours and the order are the screen's (BoardFrame::dots): on a split screen each
        // player keeps one colour on every map and the others stay orange.
        for (const BoardDot& d : f.dots)
            if (d.racer >= 0 && d.racer < f.session->racer_count())
                dot(f.session->car(d.racer), d.glyph);
    }

    // Bottom left: the damage gauge, with the game's own "DAMAGE:" and "%".
    {
        const float y = height - margin - hud_.sprite_height(g::kDamageWord) * 0.7f * k;
        const float dds = 0.7f * k;
        float pen = margin;
        hud_.sprite(g::kDamageWord, pen, y, dds);
        pen += hud_.sprite_width(g::kDamageWord) * dds + 6.0f * k;
        const int pct = std::clamp(car.damage_percent, 0, 99);
        if (pct >= 10) {
            hud_.sprite(g::kSmallDigit0 + pct / 10, pen, y, dds);
            pen += hud_.sprite_width(g::kSmallDigit0 + pct / 10) * dds + 2.0f * k;
        }
        hud_.sprite(g::kSmallDigit0 + pct % 10, pen, y, dds);
        pen += hud_.sprite_width(g::kSmallDigit0 + pct % 10) * dds + 2.0f * k;
        hud_.sprite(g::kPercent, pen, y, dds);
    }

    // Bottom right: speed, and the nitro reserve as a bar.
    {
        // The ORIGINAL'S OWN CALIBRATION, off its cabin speedometers: the JEEP's dial reads 120 at
        // a `vfwd` of 35000 (`ndial point 0, ..., 35000`). The first scale
        // here was a guess (200 world units to the metre) that read about a quarter of that; the
        // user, who knows the game, judged the dial right - "at 45 you feel at least twice that".
        constexpr int kDialTop = 120, kDialVfwd = 35000;
        int kmh = static_cast<int>(static_cast<int64_t>(car.vfwd) * kDialTop / kDialVfwd);
        if (kmh < 0) kmh = -kmh;
        const float unit_w = hud_.text_width(" KM/H", fs);
        const float nw = hud_.number_width(kmh, ds);
        const float sy = height - margin - digit_h;
        hud_.number(kmh, width - margin - unit_w - nw, sy, ds);
        hud_.text("KM/H", width - margin - unit_w + hud_.text_width(" ", fs), sy + label_dy, fs,
                  1.0f, 1.0f, 1.0f);
        // ⚠️ THE BOOST HAS TWO SHAPES, and they are the vehicle's two boost MODES, not a choice
        // of presentation (the `nitros` line):
        //
        //   mode 2, HOLDABLE - the reserve is a stock of TICKS that drains while the button is
        //     down, so it draws as a bar that empties;
        //   mode 1, PULSE - the reserve is a COUNT of fixed bursts, each 200 ticks, so it draws
        //     as that many pips, one going out per use.
        //
        // Reported by the user, and the data says the same thing: `nitro[0]` is 1 or 2 and
        // `nitros <n>,<t>` carries the two scales side by side.
        const float gy = sy - 8.0f * k;
        if (f.tuning->nitro[0] == 2 && f.nitros->full_scale_ticks > 0) {
            const float bw = 160.0f * k, bh = 10.0f * k;
            const float bx = width - margin - bw;
            hud_.rect(bx, gy - bh, bw, bh, 0.1f, 0.1f, 0.1f, 0.6f);
            const float frac = static_cast<float>(car.nitro_fuel) / f.nitros->full_scale_ticks;
            hud_.rect(bx, gy - bh, bw * (frac < 0 ? 0 : (frac > 1 ? 1 : frac)), bh, 1.0f, 0.55f,
                      0.1f, 0.9f);
        } else if (f.tuning->nitro[0] == 1 && f.nitros->count > 0) {
            // The orange mini-map dot is the roundest thing the game's own HUD set has.
            const int pip = g::kDot0 + 4;
            const float ps = 0.55f * k;
            const float pw = hud_.sprite_width(pip) * ps;
            const float step = pw + 5.0f * k;
            const float ph = hud_.sprite_height(pip) * ps;
            for (int i = 0; i < f.nitros->count; ++i) {
                const float px = width - margin - (f.nitros->count - i) * step + 5.0f * k;
                hud_.sprite(pip, px, gy - ph, ps, i < car.nitro_fuel ? 1.0f : 0.22f);
            }
        }
    }

    // Centre: the banners the game has artwork for. ⚠️ NO COUNTDOWN NUMBER - the original does
    // not print one. The count is the start gantry's lights going red, red+amber, amber, green,
    // which is drawn above in the world, not here.
    {
        // FINAL LAP for five seconds from the line, then gone (user, 2026-09-25): held for the
        // whole lap it covered the road.
        if (me.lap + 1 == f.session->race().laps() && !me.finished) {
            if ((f.session->race().race_ticks() - me.lap_start_tick) * kGameTickMs < 5000u)
                hud_.sprite_centred(g::kFinalLap, width * 0.5f, height * 0.12f, 1.0f * k);
        }
        // ⚠️ The game HAS artwork for this, and now something to draw it for: crossing the line
        // without every checkpoint costs the lap. Held the way the original holds its own
        // banners - 0x15e of its 14 ms ticks, which is what race_ticks() counts.
        //
        // Both warnings BLINK, 500 ms shown and 500 ms hidden (user, 2026-09-27), phased from the
        // moment each went up so the first half-second is always visible. Game ticks, which are
        // a clock: the blink is the same at any frame rate.
        auto blink_on = [](uint32_t game_ticks_since) {
            return (game_ticks_since * kGameTickMs / 500u) % 2u == 0u;
        };
        if (me.missed_check && f.session->race().race_ticks() - me.missed_check_tick < 350u &&
            blink_on(f.session->race().race_ticks() - me.missed_check_tick))
            hud_.sprite_centred(g::kMissedCheck, width * 0.5f, height * 0.24f, 1.0f * k);
        // WRONG WAY, for as long as it lasts: the count resets the moment the car makes any
        // forward progress round the ring, so the banner goes out by itself. The count goes on
        // climbing, two game ticks a physics tick, while it is up - which dates the banner.
        // Not on a wreck: it is going nowhere, and says so already. Nor past the line: the race
        // is over for this car, whichever way it points (user, 2026-09-30).
        if (me.wrong_way && car.damage >= 0 && !me.finished &&
            blink_on(static_cast<uint32_t>(me.wrong_way_ticks - kWrongWayTicks)))
            hud_.sprite_centred(g::kWrongWay, width * 0.5f, height * 0.18f, 1.0f * k);
        // Split screen: this quarter follows someone else now (RaceScreen::watch_for).
        if (!f.watching.empty())
            hud_.text_centred(f.watching, width * 0.5f, height * 0.62f, 2.0f * k, 1.0f, 1.0f, 0.4f);
        // At the TOP, in FINAL LAP's place (user, 2026-09-30): these stay up for the rest of the
        // race, spectating included, and in the middle they covered what there is to watch.
        if (me.finished)
            hud_.text_centred("FINISHED", width * 0.5f, height * 0.12f, 3.5f * k, 1.0f, 1.0f,
                              0.4f);
        // Ours: the original shows a wreck by its 100% gauge and nothing else. Said here
        // because ALT no longer answers, and a player pressing it deserves to know why.
        else if (car.damage < 0)
            hud_.text_centred("WRECKED", width * 0.5f, height * 0.12f, 3.5f * k, 1.0f, 0.35f,
                              0.25f);
        // The count to the results (user, 2026-09-27): whole seconds, rounded up, so it reads
        // 5 4 3 2 1 and the table comes on the tick the 1 would have run out.
        if (f.results_in > 0)
            hud_.text_centred("RESULTS IN " + std::to_string(f.results_in), width * 0.5f,
                              height * 0.52f, 2.0f * k, 1.0f, 1.0f, 1.0f);
    }
    // A network race's state, one line under the board (RaceScreen::network_status).
    if (!f.status.empty())
        hud_.text_centred(f.status, width * 0.5f, height * 0.62f, 2.0f * k, f.status_rgb[0],
                          f.status_rgb[1], f.status_rgb[2]);

    hud_.flush();
}

// Three players: the fourth corner is the race seen from above - the level's own mini-map,
// blown up to fill almost the whole corner, with every car on it (user, 2026-09-26).
void RaceBoard::draw_race_map(const RaceSession& session, const std::vector<BoardDot>& dots, int w,
                              int h) {
    if (!hud_.ready()) return;
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    hud_.begin(w, h);
    if (hud_.has_minimap() && topmap_.valid() && hud_.minimap_width() > 0) {
        // Almost all of the corner: nine tenths of its smaller side. Not rounded to a whole
        // magnification, unlike the board's own map - here the size is the point.
        const float ms = 0.9f * std::min(static_cast<float>(w) / hud_.minimap_width(),
                                         static_cast<float>(h) / hud_.minimap_height());
        const float mx = (w - hud_.minimap_width() * ms) * 0.5f;
        const float my = (h - hud_.minimap_height() * ms) * 0.5f;
        hud_.minimap(mx, my, ms, 0.95f);
        const float ds = std::max(1.0f, ms * 0.5f);
        const auto dot = [&](const VehicleSim& car_, int glyph) {
            int px = 0, py = 0;
            topmap_pixel(topmap_, car_.world_x(), car_.world_z(), px, py);
            const float dw = static_cast<float>(hud_.sprite_width(glyph)) * ds;
            const float dh = static_cast<float>(hud_.sprite_height(glyph)) * ds;
            hud_.sprite(glyph, mx + px * ms - dw * 0.5f, my + py * ms - dh * 0.5f, ds);
        };
        // The others first, the players last, so no player is hidden under a ghost - the
        // screen's order (BoardDot).
        for (const BoardDot& d : dots)
            if (d.racer >= 0 && d.racer < session.racer_count()) dot(session.car(d.racer), d.glyph);
    }
    hud_.flush();
}

// The question over everything, split views included: the whole target, dimmed.
void RaceBoard::draw_quit_confirm(int width, int height, bool network) {
    if (!hud_.ready()) return;
    const float k = static_cast<float>(height) / 480.0f;
    hud_.begin(width, height);
    hud_.rect(0.0f, height * 0.36f, static_cast<float>(width), height * 0.26f, 0.0f, 0.0f, 0.0f,
              0.75f);
    hud_.text_centred(network ? "LEAVE THE GAME?" : "LEAVE THE RACE?", width * 0.5f, height * 0.40f,
                      3.0f * k, 1.0f, 0.87f, 0.25f);
    hud_.text_centred("ENTER / A: YES     ESC / B: NO", width * 0.5f, height * 0.50f, 1.6f * k,
                      1.0f, 1.0f, 1.0f);
    if (network)
        hud_.text_centred("THE RACE GOES ON MEANWHILE", width * 0.5f, height * 0.56f, 1.4f * k,
                          0.63f, 0.63f, 0.75f);
    hud_.flush();
}

// The whole target, black, at `fade` - over the race, its HUD and the split views alike.
void RaceBoard::draw_fade(int width, int height, float fade) {
    if (fade <= 0.0f || !hud_.ready()) return;
    hud_.begin(width, height);
    hud_.rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f,
              0.0f, fade);
    hud_.flush();
}

} // namespace bigred
