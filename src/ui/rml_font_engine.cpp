#include "rml_font_engine.h"

#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>   // StringView, which Types.h only forward-declares

#include <algorithm>
#include <cstdio>
#include <unordered_map>

#include "formats/game_files.h"
#include "formats/localised_text.h" // interface_glyph, next_code_point
#include "ui/rml_render_interface.h"

namespace bigred {
namespace {

// The name the atlas is registered under with the renderer, and the source RmlUi then asks for.
// It is not a path on purpose: nothing on disk answers to it.
constexpr const char* kAtlasSource = "bigred:font-atlas";

constexpr int kGlyphsPerRow = 16;

// The wibbly lettering: five drawings, one every 5 ticks of 14 ms (`anim 5,5,7, ...`), and the
// second family two frames ahead (`4,5,6,2,3`).
constexpr uint32_t kWibFrameMs = 70;
constexpr int kWibFrames = 5;
constexpr int kWibPhaseB = 2;

uint32_t g_clock_ms = 0;

} // namespace

void RmlFontEngine::set_clock_ms(uint32_t ms) { g_clock_ms = ms; }

bool RmlFontEngine::load(const std::string& game_dir, RmlRenderInterface& renderer) {
    ArchivePtr always_archive;
    try {
        always_archive = game_archive(game_dir, "ALWAYS.WAP");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Menus: no ALWAYS.WAP (%s) - the interface will have no text.\n",
                     ex.what());
        return false;
    }
    const Archive& always = *always_archive;
    auto col = always.find("DEFAULT.COL");
    if (col == always.end()) return false;
    const std::vector<RGB> palette = load_palette_rgb(col->second.data(), col->second.size());
    if (palette.empty()) return false;

    // Every alphabet the menus use: SML_ALPH first (sheet 0 is the plain face, and nothing
    // else is required), then the five wibbly drawings when they are all there.
    std::vector<std::vector<SpriteGlyph>> fonts;
    auto decode = [&](const char* name) {
        auto it = always.find(name);
        if (it == always.end()) return std::vector<SpriteGlyph>();
        return decode_spr(it->second.data(), it->second.size());
    };
    fonts.push_back(decode("SML_ALPH.SPR"));
    if (fonts[0].empty()) return false;
    {
        std::vector<std::vector<SpriteGlyph>> wibs;
        for (const char* n : {"WIBS1.SPR", "WIBS2.SPR", "WIBS3.SPR", "WIBS4.SPR", "WIBS5.SPR"}) {
            wibs.push_back(decode(n));
            if (wibs.back().empty()) { wibs.clear(); break; }
        }
        for (auto& w : wibs) fonts.push_back(std::move(w));
    }

    // ONE atlas, each alphabet a plain grid of its own cell size, stacked. A few hundred
    // glyphs is well under a megabyte; shelf packing would save nothing worth the code.
    struct Layout { int cell_w = 0, cell_h = 0, y0 = 0, rows = 0; };
    std::vector<Layout> lay(fonts.size());
    int atlas_w = 0, atlas_h = 0;
    for (size_t s = 0; s < fonts.size(); ++s) {
        for (const SpriteGlyph& g : fonts[s]) {
            if (!g.valid()) continue;
            lay[s].cell_w = std::max(lay[s].cell_w, g.width);
            lay[s].cell_h = std::max(lay[s].cell_h, g.height);
        }
        lay[s].rows = (static_cast<int>(fonts[s].size()) + kGlyphsPerRow - 1) / kGlyphsPerRow;
        lay[s].y0 = atlas_h;
        atlas_h += lay[s].cell_h * lay[s].rows;
        atlas_w = std::max(atlas_w, lay[s].cell_w * kGlyphsPerRow);
    }
    if (atlas_w <= 0 || atlas_h <= 0) return false;
    std::vector<uint8_t> rgba(static_cast<size_t>(atlas_w) * atlas_h * 4, 0);

    sheets_.assign(fonts.size(), Sheet());
    for (size_t s = 0; s < fonts.size(); ++s) {
        Sheet& sheet = sheets_[s];
        sheet.rows = lay[s].cell_h;
        sheet.glyphs.assign(fonts[s].size(), Glyph());
        for (size_t i = 0; i < fonts[s].size(); ++i) {
            const SpriteGlyph& g = fonts[s][i];
            const int cx = static_cast<int>(i % kGlyphsPerRow) * lay[s].cell_w;
            const int cy = lay[s].y0 + static_cast<int>(i / kGlyphsPerRow) * lay[s].cell_h;
            Glyph& out = sheet.glyphs[i];
            // A blank glyph still has to advance the pen, or every space closes up. `field3`
            // is the file's own advance; the fallback is a third of the cell.
            out.advance = g.width > 0 ? g.width : (g.field3 > 0 ? g.field3 : lay[s].cell_h / 3);
            out.width = g.width;
            out.height = g.height;
            if (!g.valid()) continue;
            for (int y = 0; y < g.height; ++y) {
                for (int x = 0; x < g.width; ++x) {
                    const uint8_t index = g.pixels[static_cast<size_t>(y) * g.width + x];
                    if (index == 0) continue;   // palette index 0 is transparent, everywhere
                    const RGB& c = palette[index < palette.size() ? index : 0];
                    const size_t o = (static_cast<size_t>(cy + y) * atlas_w + (cx + x)) * 4;
                    rgba[o] = c.r;
                    rgba[o + 1] = c.g;
                    rgba[o + 2] = c.b;
                    rgba[o + 3] = 255;
                }
            }
            out.u0 = static_cast<float>(cx) / atlas_w;
            out.v0 = static_cast<float>(cy) / atlas_h;
            out.u1 = static_cast<float>(cx + g.width) / atlas_w;
            out.v1 = static_cast<float>(cy + g.height) / atlas_h;
            out.ok = true;
        }
    }

    renderer.register_image(kAtlasSource, std::move(rgba), atlas_w, atlas_h);
    ready_ = true;
    std::printf("Menus: %zu alphabet(s) (SML_ALPH%s) in a %dx%d atlas.\n", sheets_.size(),
                sheets_.size() > 1 ? " + WIBS1..5" : "", atlas_w, atlas_h);
    return true;
}

bool RmlFontEngine::LoadFontFace(const Rml::String&, int, bool, Rml::Style::FontWeight) {
    // The typefaces are loaded from the game's archives in load(), not from a file a
    // stylesheet names. Saying yes keeps an `@font-face` rule from logging an error.
    return ready_;
}

Rml::FontFaceHandle RmlFontEngine::GetFontFaceHandle(const Rml::String& family,
                                                     Rml::Style::FontStyle,
                                                     Rml::Style::FontWeight, int size) {
    if (!ready_) return 0;
    Kind kind = Kind::Plain;
    if (sheets_.size() > 1) {
        if (family == "wibs") kind = Kind::Wibs;
        else if (family == "wibs-b") kind = Kind::WibsB;
    }
    const int rows = sheets_[kind == Kind::Plain ? 0 : 1].rows;
    // ⚠️ WHOLE-NUMBER MAGNIFICATION, ROUNDED DOWN. Half a source pixel per output pixel is a
    // smear, and this lettering's whole legibility is its one-pixel outline.
    int scale = rows > 0 ? size / rows : 1;
    if (scale < 1) scale = 1;

    for (size_t i = 0; i < faces_.size(); ++i)
        if (faces_[i].scale == scale && faces_[i].kind == kind)
            return static_cast<Rml::FontFaceHandle>(i + 1);

    Face f;
    f.kind = kind;
    f.scale = scale;
    const float h = static_cast<float>(rows * scale);
    f.metrics.size = rows * scale;
    // The artwork has no baseline of its own: it is a cell, drawn from its top. Treating the
    // whole cell as ascent puts that top exactly where RmlUi expects the ascent to start.
    f.metrics.ascent = h;
    f.metrics.descent = 0.0f;
    f.metrics.line_spacing = h + static_cast<float>(scale) * 2.0f;
    f.metrics.x_height = h * 0.6f;
    f.metrics.underline_position = static_cast<float>(scale);
    f.metrics.underline_thickness = static_cast<float>(scale);
    f.metrics.has_ellipsis = false;
    faces_.push_back(f);
    return static_cast<Rml::FontFaceHandle>(faces_.size());
}

RmlFontEngine::Face* RmlFontEngine::face_for(Rml::FontFaceHandle handle) {
    const size_t i = static_cast<size_t>(handle);
    if (i == 0 || i > faces_.size()) return nullptr;
    return &faces_[i - 1];
}

const RmlFontEngine::Sheet& RmlFontEngine::sheet_for(const Face& f) const {
    if (f.kind == Kind::Plain || sheets_.size() <= 1) return sheets_[0];
    int frame = static_cast<int>((g_clock_ms / kWibFrameMs) % kWibFrames);
    if (f.kind == Kind::WibsB) frame = (frame + kWibPhaseB) % kWibFrames;
    return sheets_[static_cast<size_t>(1 + frame)];
}

// Both alphabets are capitals only, with the accented capitals after `Z` - interface_glyph
// (formats/localised_text.h) says which glyph a character is, per alphabet.
int RmlFontEngine::glyph_of(const Face& f, char32_t c) const {
    const int i = interface_glyph(c, f.kind != Kind::Plain);
    const Sheet& s = sheets_[f.kind == Kind::Plain ? 0 : std::min<size_t>(1, sheets_.size() - 1)];
    return (i >= 0 && i < static_cast<int>(s.glyphs.size())) ? i : -1;
}

const Rml::FontMetrics& RmlFontEngine::GetFontMetrics(Rml::FontFaceHandle handle) {
    static const Rml::FontMetrics none{};
    Face* f = face_for(handle);
    return f ? f->metrics : none;
}

int RmlFontEngine::GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string,
                                  const Rml::TextShapingContext&, Rml::Character) {
    Face* f = face_for(handle);
    if (!f) return 0;
    // The five wibbly drawings share their widths, so any
    // frame measures the same - the text does not reflow as it wobbles.
    const Sheet& sheet = sheet_for(*f);
    const int gap = f->kind == Kind::Plain ? 1 : 0;
    int w = 0;
    for (size_t at = 0; at < string.size();) {
        const int i = glyph_of(*f, next_code_point(string.begin(), string.size(), at));
        if (i < 0) continue;
        w += (sheet.glyphs[static_cast<size_t>(i)].advance + gap) * f->scale;
    }
    return w;
}

int RmlFontEngine::GenerateString(Rml::RenderManager& render_manager,
                                  Rml::FontFaceHandle face_handle, Rml::FontEffectsHandle,
                                  Rml::StringView string, Rml::Vector2f position,
                                  Rml::ColourbPremultiplied colour, float,
                                  const Rml::TextShapingContext&,
                                  Rml::TexturedMeshList& mesh_list) {
    Face* f = face_for(face_handle);
    if (!f) return 0;
    const Sheet& sheet = sheet_for(*f);
    const int gap = f->kind == Kind::Plain ? 1 : 0;

    mesh_list.push_back(Rml::TexturedMesh());
    Rml::TexturedMesh& out = mesh_list.back();
    out.texture = render_manager.LoadTexture(kAtlasSource);

    // ⚠️ `position` IS THE BASELINE, and this lettering is drawn from the top of its cell - so
    // the whole cell hangs above the baseline, which is exactly what the ascent above declares.
    const float top = position.y - f->metrics.ascent;
    float pen = position.x;
    for (size_t at = 0; at < string.size();) {
        const int i = glyph_of(*f, next_code_point(string.begin(), string.size(), at));
        if (i < 0) continue;
        const Glyph& g = sheet.glyphs[static_cast<size_t>(i)];
        if (g.ok) {
            Rml::MeshUtilities::GenerateQuad(
                out.mesh, Rml::Vector2f(pen, top),
                Rml::Vector2f(static_cast<float>(g.width * f->scale),
                              static_cast<float>(g.height * f->scale)),
                colour, Rml::Vector2f(g.u0, g.v0), Rml::Vector2f(g.u1, g.v1));
        }
        pen += (g.advance + gap) * f->scale;
    }
    return static_cast<int>(pen - position.x);
}

int RmlFontEngine::GetVersion(Rml::FontFaceHandle handle) {
    // A wibbly face has new glyphs every frame of its animation: say so, and RmlUi regenerates
    // every text drawn with it. The plain face never changes.
    const Face* f = face_for(handle);
    if (!f || f->kind == Kind::Plain) return 0;
    return static_cast<int>(g_clock_ms / kWibFrameMs);
}

} // namespace bigred
