#include "hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shader.h"
#include "formats/localised_text.h" // interface_glyph, next_code_point

namespace bigred {
namespace {

// A 2D overlay in PIXEL coordinates: the vertex shader does the whole projection, so nothing
// upstream has to know about NDC. Top-left origin, y down, which is how a layout reads.
const char* kHudVertexShader = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform vec2 uScreen;
out vec2 vUV;
out vec4 vColor;
void main() {
    vec2 ndc = vec2(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

const char* kHudFragmentShader = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uAtlas;
out vec4 FragColor;
void main() {
    vec4 t = texture(uAtlas, vUV);
    FragColor = t * vColor;
    if (FragColor.a < 0.01) discard;
}
)";

constexpr int kAtlasPad = 1;

} // namespace

Hud::~Hud() {
    if (texture_) glDeleteTextures(1, &texture_);
    if (vbo_) gl::DeleteBuffers(1, &vbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    if (program_) gl::DeleteProgram(program_);
}

bool Hud::build(const std::vector<SpriteGlyph>& sprites, const std::vector<SpriteGlyph>& font,
                bool font_indexed, const std::vector<RGB>& palette, const SpriteGlyph* minimap,
                const std::vector<RGB>* minimap_palette) {
    if (sprites.empty() && font.empty()) return false;
    minimap_ = Entry();
    font_indexed_ = font_indexed;
    font_rows_ = kFntRows;
    for (const SpriteGlyph& g : font) {
        if (g.valid()) { font_rows_ = g.height; break; }
    }

    // Shelf packing, widest first. The whole set is a few hundred kilobytes of glyph; there is
    // no reason for anything cleverer, and a stable layout makes a bad atlas easy to eyeball.
    struct Item { const SpriteGlyph* g; int list; int index; };
    std::vector<Item> items;
    for (size_t i = 0; i < sprites.size(); ++i)
        if (sprites[i].valid()) items.push_back({&sprites[i], 0, static_cast<int>(i)});
    for (size_t i = 0; i < font.size(); ++i)
        if (font[i].valid()) items.push_back({&font[i], 1, static_cast<int>(i)});
    if (minimap && minimap->valid()) items.push_back({minimap, 2, 0});
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.g->height != b.g->height) return a.g->height > b.g->height;
        return a.list == b.list ? a.index < b.index : a.list < b.list;
    });

    int atlas_w = 512;
    for (const Item& it : items) atlas_w = std::max(atlas_w, it.g->width + 2 * kAtlasPad + 2);
    // One extra opaque pixel at the very start, for `rect`.
    int pen_x = 2, pen_y = kAtlasPad, shelf_h = 1;
    struct Placed { int x, y; };
    std::vector<Placed> placed(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const SpriteGlyph& g = *items[i].g;
        if (pen_x + g.width + kAtlasPad > atlas_w) {
            pen_x = kAtlasPad;
            pen_y += shelf_h + kAtlasPad;
            shelf_h = 0;
        }
        placed[i] = {pen_x, pen_y};
        pen_x += g.width + kAtlasPad;
        shelf_h = std::max(shelf_h, g.height);
    }
    const int atlas_h = pen_y + shelf_h + kAtlasPad;

    std::vector<uint8_t> rgba(static_cast<size_t>(atlas_w) * atlas_h * 4, 0);
    // The white pixel, at (0,0).
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 255;

    sprites_.assign(sprites.size(), Entry());
    font_.assign(font.size(), Entry());
    for (size_t i = 0; i < items.size(); ++i) {
        const SpriteGlyph& g = *items[i].g;
        const int ox = placed[i].x, oy = placed[i].y;
        for (int y = 0; y < g.height; ++y) {
            for (int x = 0; x < g.width; ++x) {
                const uint8_t idx = g.pixels[static_cast<size_t>(y) * g.width + x];
                if (idx == 0) continue;  // the game's transparent index, everywhere
                uint8_t r = 255, gr = 255, b = 255;
                if (items[i].list == 2) {
                    const std::vector<RGB>& pal = minimap_palette ? *minimap_palette : palette;
                    if (static_cast<size_t>(idx) < pal.size()) {
                        r = pal[idx].r;
                        gr = pal[idx].g;
                        b = pal[idx].b;
                    }
                } else if (items[i].list == 0 || font_indexed_) {
                    // A sprite carries palette indices, and so does SML_ALPH - which is the
                    // whole point of it: index 19 is the black outline that makes the text
                    // readable over the terrain. MICRO.FNT alone is a bare mask.
                    if (static_cast<size_t>(idx) < palette.size()) {
                        r = palette[idx].r;
                        gr = palette[idx].g;
                        b = palette[idx].b;
                    }
                }
                const size_t o = (static_cast<size_t>(oy + y) * atlas_w + (ox + x)) * 4;
                rgba[o + 0] = r;
                rgba[o + 1] = gr;
                rgba[o + 2] = b;
                rgba[o + 3] = 255;
            }
        }
        Entry e;
        e.u0 = static_cast<float>(ox) / atlas_w;
        e.v0 = static_cast<float>(oy) / atlas_h;
        e.u1 = static_cast<float>(ox + g.width) / atlas_w;
        e.v1 = static_cast<float>(oy + g.height) / atlas_h;
        e.w = g.width;
        e.h = g.height;
        e.advance = g.field3 > 0 ? g.field3 : g.width;
        e.ok = true;
        if (items[i].list == 2)
            minimap_ = e;
        else
            (items[i].list == 0 ? sprites_ : font_)[static_cast<size_t>(items[i].index)] = e;
    }
    // A blank glyph still has to advance the pen, or every space closes up.
    for (size_t i = 0; i < font.size(); ++i) {
        if (!font_[i].ok) font_[i].advance = font[i].field3 > 0 ? font[i].field3 : 3;
    }
    // The middle of the opaque pixel at (0,0). The V pair divides by the atlas HEIGHT, which
    // is not the width - the blocks `rect` draws (the gauges, the clock's separators) come out
    // of here and must not pick up a neighbouring glyph.
    white_.u0 = 0.25f / atlas_w;
    white_.u1 = 0.75f / atlas_w;
    white_.v0 = 0.25f / atlas_h;
    white_.v1 = 0.75f / atlas_h;
    white_.w = white_.h = 1;
    white_.ok = true;

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas_w, atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    // ⚠️ NEAREST, both ways. Every glyph in here is 1996 pixel art a few pixels tall; bilinear
    // magnification turns it into a grey smear, and bilinear minification bleeds the
    // neighbouring glyph in across the atlas padding.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    glBindTexture(GL_TEXTURE_2D, 0);

    program_ = Shader::compile(kHudVertexShader, kHudFragmentShader);
    if (!program_) {
        std::fprintf(stderr, "HUD shader failed to compile.\n");
        return false;
    }
    screen_loc_ = gl::GetUniformLocation(program_, "uScreen");
    tex_loc_ = gl::GetUniformLocation(program_, "uAtlas");

    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);
    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(GL_ARRAY_BUFFER_, vbo_);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            reinterpret_cast<void*>(offsetof(Vertex, u)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            reinterpret_cast<void*>(offsetof(Vertex, r)));
    gl::EnableVertexAttribArray(2);
    gl::BindVertexArray(0);
    return true;
}

void Hud::begin(int screen_w, int screen_h) {
    screen_w_ = screen_w > 0 ? screen_w : 1;
    screen_h_ = screen_h > 0 ? screen_h : 1;
    batch_.clear();
}

void Hud::quad(const Entry& e, float x, float y, float w, float h, float r, float g, float b,
               float a) {
    if (!e.ok) return;
    // Snap to whole screen pixels. A glyph whose left edge lands at x.5 is sampled half a texel
    // off along its whole width, and with NEAREST that drops or doubles a column: the stem of a
    // `1` disappears. Rounding the size too keeps the magnification uniform across the string.
    x = std::floor(x + 0.5f);
    y = std::floor(y + 0.5f);
    w = std::floor(w + 0.5f);
    h = std::floor(h + 0.5f);
    const Vertex tl{x, y, e.u0, e.v0, r, g, b, a};
    const Vertex tr{x + w, y, e.u1, e.v0, r, g, b, a};
    const Vertex bl{x, y + h, e.u0, e.v1, r, g, b, a};
    const Vertex br{x + w, y + h, e.u1, e.v1, r, g, b, a};
    batch_.push_back(tl);
    batch_.push_back(bl);
    batch_.push_back(br);
    batch_.push_back(tl);
    batch_.push_back(br);
    batch_.push_back(tr);
}

void Hud::sprite(int index, float x, float y, float scale, float alpha) {
    if (index < 0 || index >= static_cast<int>(sprites_.size())) return;
    const Entry& e = sprites_[static_cast<size_t>(index)];
    quad(e, x, y, e.w * scale, e.h * scale, 1.0f, 1.0f, 1.0f, alpha);
}

void Hud::minimap(float x, float y, float scale, float alpha) {
    if (!minimap_.ok) return;
    quad(minimap_, x, y, minimap_.w * scale, minimap_.h * scale, 1.0f, 1.0f, 1.0f, alpha);
}

void Hud::sprite_centred(int index, float cx, float y, float scale, float alpha) {
    if (index < 0 || index >= static_cast<int>(sprites_.size())) return;
    sprite(index, cx - sprites_[static_cast<size_t>(index)].w * scale * 0.5f, y, scale, alpha);
}

int Hud::sprite_width(int index) const {
    if (index < 0 || index >= static_cast<int>(sprites_.size())) return 0;
    return sprites_[static_cast<size_t>(index)].w;
}
int Hud::sprite_height(int index) const {
    if (index < 0 || index >= static_cast<int>(sprites_.size())) return 0;
    return sprites_[static_cast<size_t>(index)].h;
}

float Hud::text_width(const std::string& s, float scale) const {
    const float ts = text_scale(scale);
    float w = 0;
    for (size_t at = 0; at < s.size();) {
        const int i = interface_glyph(next_code_point(s.data(), s.size(), at), false);
        if (i < 0 || i >= static_cast<int>(font_.size())) continue;
        w += (font_[static_cast<size_t>(i)].advance + 1) * ts;
    }
    return w;
}

void Hud::text(const std::string& s, float x, float y, float scale, float r, float g, float b,
               float a) {
    // Both candidate fonts are uppercase only, deliberately - see sprite.h. Fold rather than
    // drop, or any lowercase in a caller's string vanishes.
    const float ts = text_scale(scale);
    float pen = x;
    for (size_t at = 0; at < s.size();) {
        const int i = interface_glyph(next_code_point(s.data(), s.size(), at), false);
        if (i < 0 || i >= static_cast<int>(font_.size())) continue;
        const Entry& e = font_[static_cast<size_t>(i)];
        if (e.ok) quad(e, pen, y, e.w * ts, e.h * ts, r, g, b, a);
        pen += (e.advance + 1) * ts;
    }
}

// ------------------------------------------------------------------- the numeric readouts
//
// Everything below draws with GAMESVGA's small digit set and with the atlas's white pixel, and
// nothing else: one typeface for every number the board shows. See the header for why the
// digits are monospaced.

float Hud::digit_cell(float scale) const {
    int widest = 0;
    for (int d = 0; d < 10; ++d) widest = std::max(widest, sprite_width(game_spr::kSmallDigit0 + d));
    if (widest <= 0) return 0;
    return (widest + 4) * scale;
}

float Hud::number_width(int value, float scale, int min_digits) const {
    if (value < 0) value = 0;
    int digits = 0;
    for (int v = value; v > 0; v /= 10) ++digits;
    if (digits < min_digits) digits = min_digits;
    if (digits < 1) digits = 1;
    return digits * digit_cell(scale);
}

float Hud::number(int value, float x, float y, float scale, int min_digits, float alpha) {
    if (value < 0) value = 0;
    int digits = 0;
    for (int v = value; v > 0; v /= 10) ++digits;
    if (digits < min_digits) digits = min_digits;
    if (digits < 1) digits = 1;
    const float cell = digit_cell(scale);
    float pen = x;
    for (int k = digits - 1; k >= 0; --k) {
        int div = 1;
        for (int j = 0; j < k; ++j) div *= 10;
        const int d = (value / div) % 10;
        const int idx = game_spr::kSmallDigit0 + d;
        // Centred in its cell: that is what makes a narrow `1` sit under a wide `0` instead of
        // dragging the rest of the number sideways with it.
        sprite(idx, pen + (cell - sprite_width(idx) * scale) * 0.5f, y, scale, alpha);
        pen += cell;
    }
    return pen;
}

float Hud::separator_width(char c, float scale) const {
    const float cell = digit_cell(scale);
    switch (c) {
        case ':': return cell * 0.34f;
        case '.': return cell * 0.30f;
        case '/': return cell * 0.52f;
        default: return cell * 0.30f;
    }
}

float Hud::separator(char c, float x, float y, float scale, float r, float g, float b, float a) {
    const float h = sprite_height(game_spr::kSmallDigit0) * scale;
    // One block, at the digits' own stroke weight. The artwork's stems are about a sixth of a
    // glyph's height, so that is what a dot is made of.
    const float s = std::max(2.0f, h * 0.16f);
    const float w = separator_width(c, scale);
    switch (c) {
        case ':':
            rect(x + (w - s) * 0.5f, y + h * 0.34f - s * 0.5f, s, s, r, g, b, a);
            rect(x + (w - s) * 0.5f, y + h * 0.74f - s * 0.5f, s, s, r, g, b, a);
            break;
        case '.':
            rect(x + (w - s) * 0.5f, y + h - s * 1.4f, s, s, r, g, b, a);
            break;
        case '/': {
            // A staircase of the same blocks, bottom-left to top-right. The digit artwork has
            // no slash and borrowing one from the small alphabet puts two typefaces in one
            // number; five blocks read as a slash and stay in this one.
            const int steps = 5;
            for (int i = 0; i < steps; ++i) {
                const float t = static_cast<float>(i) / (steps - 1);
                rect(x + (w - s) * t, y + h * 0.9f - (h * 0.75f) * t - s, s, s, r, g, b, a);
            }
            break;
        }
        default: break;
    }
    return x + w;
}

float Hud::clock_width(float scale) const {
    return digit_cell(scale) * 5 + separator_width(':', scale) + separator_width('.', scale);
}

void Hud::clock(uint32_t ms, float x, float y, float scale, float alpha) {
    const uint32_t minutes = ms / 60000u;
    const uint32_t seconds = (ms / 1000u) % 60u;
    const uint32_t hundredths = (ms / 10u) % 100u;
    float pen = x;
    pen = number(static_cast<int>(minutes), pen, y, scale, 1, alpha);
    pen = separator(':', pen, y, scale, 1.0f, 1.0f, 1.0f, alpha);
    pen = number(static_cast<int>(seconds), pen, y, scale, 2, alpha);
    pen = separator('.', pen, y, scale, 1.0f, 1.0f, 1.0f, alpha);
    number(static_cast<int>(hundredths), pen, y, scale, 2, alpha);
}

void Hud::text_centred(const std::string& s, float cx, float y, float scale, float r, float g,
                       float b, float a) {
    text(s, cx - text_width(s, scale) * 0.5f, y, scale, r, g, b, a);
}

void Hud::rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    quad(white_, x, y, w, h, r, g, b, a);
}

void Hud::flush() {
    if (batch_.empty() || !ready()) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::UseProgram(program_);
    gl::Uniform1i(tex_loc_, 0);
    gl::Uniform2f(screen_loc_, static_cast<float>(screen_w_), static_cast<float>(screen_h_));
    glBindTexture(GL_TEXTURE_2D, texture_);
    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER_, vbo_);
    gl::BufferData(GL_ARRAY_BUFFER_, static_cast<GLsizeiptr>(batch_.size() * sizeof(Vertex)),
                   batch_.data(), GL_STREAM_DRAW_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch_.size()));
    gl::BindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    batch_.clear();
}

} // namespace bigred
