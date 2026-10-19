#include "team_logos.h"

#include <array>
#include <cstdio>
#include <unordered_map>

#include "formats/game_files.h"
#include "formats/texture.h"   // kRawPaletteBytes
#include "third_party/lodepng.h"

namespace bigred {
namespace {

// The three families, by the size that tells them apart. See the header.
constexpr int kLogoSide = 40;
constexpr int kPlateSide = 56;

bool is_size(const SpriteGlyph& g, int side) {
    return g.valid() && g.width == side && g.height == side;
}

std::string& custom_path() {
    static std::string path;
    return path;
}

} // namespace

void set_custom_logo_path(const std::string& path_utf8) { custom_path() = path_utf8; }

bool decode_logo_png(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba) {
    rgba.clear();
    if (png.empty() || png.size() > kCustomLogoMaxBytes * 64) return false;
    unsigned w = 0, h = 0;
    std::vector<unsigned char> out;
    if (lodepng::decode(out, w, h, png) != 0) return false;
    if (w != static_cast<unsigned>(kCustomLogoSide) || h != static_cast<unsigned>(kCustomLogoSide))
        return false;
    rgba.assign(out.begin(), out.end());
    return true;
}

const std::vector<uint8_t>& custom_logo_png() {
    static bool read = false;
    static std::vector<uint8_t> png;
    if (read) return png;
    read = true;
    if (custom_path().empty()) return png;
    const Blob file = read_file_utf8(custom_path());
    if (file.empty()) return png;
    std::vector<uint8_t> rgba;
    if (!decode_logo_png(file, rgba)) {
        std::fprintf(stderr, "%s: not a %dx%d PNG - the logo is ignored.\n", custom_path().c_str(),
                     kCustomLogoSide, kCustomLogoSide);
        return png;
    }
    // Re-encoded: whatever the player's editor wrote (metadata, a colour profile, no
    // compression at all), what is kept and sent is the picture and nothing else.
    std::vector<unsigned char> encoded;
    if (lodepng::encode(encoded, rgba, kCustomLogoSide, kCustomLogoSide) != 0) return png;
    if (encoded.size() > kCustomLogoMaxBytes) {
        std::fprintf(stderr, "%s: %zu bytes once packed, the limit is %zu - the logo is ignored.\n",
                     custom_path().c_str(), encoded.size(), kCustomLogoMaxBytes);
        return png;
    }
    png.assign(encoded.begin(), encoded.end());
    std::printf("Own logo: %s\n", custom_path().c_str());
    std::fflush(stdout);
    return png;
}

bool TeamLogos::load(const std::string& game_dir) {
    glyphs_.clear();
    palette_.clear();
    logo_first_ = plate_first_ = -1;
    const Archive& scn = *game_archive_or_empty(game_dir, "LOGOSCN.SCN");
    const auto spr = scn.find("LOGOS.SPR");
    const auto raw = scn.find("TEAM.RAW");
    if (spr == scn.end() || raw == scn.end() || raw->second.size() < kRawPaletteBytes) return false;
    palette_ = load_palette_rgb(raw->second.data(), kRawPaletteBytes);
    glyphs_ = decode_spr(spr->second.data(), spr->second.size());
    if (glyphs_.empty() || palette_.empty()) return false;

    // ⚠️ FOUND BY SIZE, not by a hard-coded index. The sheet has no table of contents, and a
    // literal 0 and 13 here would be two magic numbers that no reader could check against
    // anything. The first 40x40 run is the logos; the first 56x56 one is the plates.
    for (size_t i = 0; i < glyphs_.size(); ++i) {
        if (logo_first_ < 0 && is_size(glyphs_[i], kLogoSide)) logo_first_ = static_cast<int>(i);
        if (plate_first_ < 0 && is_size(glyphs_[i], kPlateSide)) plate_first_ = static_cast<int>(i);
    }

    // Each plate's own colour, read off its pixels: the commonest opaque index wins. Frame 0 of
    // each colour is enough - the five frames are the same square redrawn by hand.
    for (int c = 0; c < kTeamPlateColours; ++c) {
        int w = 0, h = 0;
        const size_t gi = static_cast<size_t>(plate_first_ + c * kTeamPlateFrames);
        if (plate_first_ < 0 || gi >= glyphs_.size()) break;
        const SpriteGlyph& g = glyphs_[gi];
        if (!g.valid()) continue;
        std::array<int, 256> hist{};
        for (const uint8_t p : g.pixels)
            if (p != 0) ++hist[p];
        int best = 0, best_n = 0;
        for (int i = 1; i < 256; ++i)
            if (hist[static_cast<size_t>(i)] > best_n) { best_n = hist[static_cast<size_t>(i)]; best = i; }
        if (best_n > 0 && static_cast<size_t>(best) < palette_.size())
            plate_colour_[c] = palette_[static_cast<size_t>(best)];
        (void)w; (void)h;
    }
    custom_rgba_.clear();
    if (builtin_count() > 0) decode_logo_png(custom_logo_png(), custom_rgba_);
    return ready();
}

int TeamLogos::count() const {
    const int n = builtin_count();
    return n > 0 && !custom_rgba_.empty() ? n + 1 : n;
}

int TeamLogos::builtin_count() const {
    if (logo_first_ < 0) return 0;
    int n = 0;
    for (size_t i = static_cast<size_t>(logo_first_); i < glyphs_.size(); ++i) {
        if (!is_size(glyphs_[i], kLogoSide)) break;
        ++n;
    }
    return n;
}

std::vector<uint8_t> TeamLogos::logo_rgba(int index, int& out_w, int& out_h) const {
    out_w = out_h = 0;
    if (logo_first_ < 0 || index < 0 || index >= count()) return {};
    if (index == builtin_count()) {
        out_w = out_h = kCustomLogoSide;
        return custom_rgba_;
    }
    const SpriteGlyph& g = glyphs_[static_cast<size_t>(logo_first_ + index)];
    out_w = g.width;
    out_h = g.height;
    return sprite_glyph_rgba(g, palette_);
}

std::vector<uint8_t> TeamLogos::plate_rgba(int colour, int frame, int& out_w, int& out_h) const {
    out_w = out_h = 0;
    if (plate_first_ < 0 || colour < 0 || colour >= kTeamPlateColours) return {};
    if (frame < 0 || frame >= kTeamPlateFrames) frame = 0;
    const size_t gi = static_cast<size_t>(plate_first_ + colour * kTeamPlateFrames + frame);
    if (gi >= glyphs_.size()) return {};
    const SpriteGlyph& g = glyphs_[gi];
    if (!is_size(g, kPlateSide)) return {};
    out_w = g.width;
    out_h = g.height;
    return sprite_glyph_rgba(g, palette_);
}

int TeamLogos::nearest_plate(uint8_t r, uint8_t g, uint8_t b) const {
    int best = 0;
    long best_d = -1;
    for (int c = 0; c < kTeamPlateColours; ++c) {
        const long dr = static_cast<long>(plate_colour_[c].r) - r;
        const long dg = static_cast<long>(plate_colour_[c].g) - g;
        const long db = static_cast<long>(plate_colour_[c].b) - b;
        const long d = dr * dr + dg * dg + db * db;
        if (best_d < 0 || d < best_d) { best_d = d; best = c; }
    }
    return best;
}

} // namespace bigred
