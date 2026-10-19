#include "globe.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <unordered_map>

#include "formats/game_files.h"
#include "formats/sprite.h"
#include "formats/texture.h"
#include "formats/threedw.h"
#include "render/shader.h"

namespace bigred {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 2.0f * kPi;

// The original's own globe, and every number here is taken from the original, not chosen.
//
// 12 slices of longitude by 8 bands of latitude, the top and bottom bands being triangular
// caps. Angles are in the game's own unit: 1024 to the turn, the same table the checkpoint
// gates use.
constexpr int kSlices = 12;
constexpr int kBands = 8;
// ⚠️ 86, NOT 1024/12 = 85.33: the original multiplies by 0x56. Twelve of them make 1032, eight units
// more than a turn, so the slices overlap very slightly. That is in the original.
constexpr int kSliceAngle = 86;
// The first latitude ring, and the step between rings: -256 and 0x40, as in the original. Ring m
// is at 768 + 64m of 1024, i.e. 270 degrees + 22.5m - a half turn from pole to pole in eight
// steps. sin() of it is the height, cos() the radius of the ring.
constexpr int kFirstRing = 768;
constexpr int kRingStep = 64;
// ⚠️ WHICH OF THE TWO READS OF THE ONE TRIG TABLE IS THE LONGITUDE'S SINE. The original has
// a single table read twice a quarter turn apart (0x200 bytes =
// 256 entries), and the LATITUDE's phase is pinned by the pole vertex it writes as a literal.
// Nothing pins the LONGITUDE's, and the two choices differ by exactly a quarter turn. This one
// is what puts all 21 positioned circuits on their own country - Australia alone in its ocean,
// Hawaii alone in the Pacific, Chile on the west coast - with `yaw_for()` left as the bare
// angle `TOURN.TXT` gives.
constexpr int kLonPhase = -256;

// One slice's 48 texture coordinates, in texels of its own 256-wide `.TM`.
//
// ⚠️ THIS IS RECORD 0 OF SIX, AND THE OTHER FIVE ARE IT PLUS 39 IN U - verified entry by
// entry against the original. Which record a slice uses is `5 - (slice % 6)`, and which texture is
// `slice < 6`; the reading order is NOT the obvious one.
//
// Vertex order per band matches the geometry below: for a quad, 0 and 1 are on the ring
// FURTHER from the first pole and 2 and 3 on the nearer one. The caps use three and the fourth
// entry is unused.
struct Uv {
    short u, v;
};
constexpr Uv kSliceUv[kBands][4] = {
    { { 38, 69}, { 47, 95}, { 31, 95}, {  0,  0} },  // band 0 - the cap at y = -1
    { { 52,122}, { 26,122}, { 31, 96}, { 47, 96} },
    { { 55,151}, { 23,151}, { 26,123}, { 52,123} },
    { { 58,178}, { 20,178}, { 22,152}, { 56,152} },
    { { 37, 28}, {  3, 28}, {  1,  0}, { 39,  0} },  // the equator is v = 178 on one side of
    { { 33, 56}, {  7, 56}, {  4, 28}, { 36, 28} },  // the seam and v = 0 on the other
    { { 28, 84}, { 12, 84}, {  7, 56}, { 33, 56} },
    { { 12, 84}, { 28, 84}, { 19,111}, {  0,  0} },  // band 7 - the cap at y = +1
};
constexpr int kUvSlicePitch = 39;   // what record s adds to every u

// The atlas the mesh samples: the two 256-wide textures side by side, so the whole globe is
// one texture and one draw call. Slices 0-5 take the left half, 6-11 the right.
constexpr int kAtlasWidth = kTmWidth * 2;
constexpr int kAtlasHeight = kTmHeight;

constexpr const char* kSphereVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
uniform float uYaw;
uniform float uPitch;
uniform vec2 uScale;
uniform vec2 uCentre;
out vec2 vUV;
out vec3 vNormal;
void main() {
    // Yaw about the poles, then pitch towards the viewer. Both in RADIANS here; the caller
    // works in turns, which is what the circuits' own coordinates are.
    // The sign matters and it is not a matter of taste: a point at longitude L must land at
    // L - uYaw, so that asking for uYaw = L brings that longitude to face the viewer. The
    // other sign sends it to 2L, which looks like a globe turning and never stops on the
    // country that was chosen.
    float cy = cos(uYaw), sy = sin(uYaw);
    vec3 p = vec3(aPos.x * cy - aPos.z * sy, aPos.y, aPos.x * sy + aPos.z * cy);
    float cp = cos(uPitch), sp = sin(uPitch);
    p = vec3(p.x, p.y * cp - p.z * sp, p.y * sp + p.z * cp);
    // ⚠️ SCREEN X IS MIRRORED (user, 2026-09-25 and 2026-09-27: "la Terre est en miroir").
    // Every circuit lands on its own country, so the mesh and the positions agree with each
    // other - they are mirrored TOGETHER, and so is the fix: the whole picture, sphere and
    // markers (Globe::draw), flips about the vertical, which puts the Americas back to the left
    // of Europe. It reverses the triangles' winding, hence the front-face culling in draw().
    p.x = -p.x;
    vNormal = p;
    vUV = aUV;
    // Orthographic, straight into canvas pixels the caller has already turned into NDC.
    gl_Position = vec4(uCentre + p.xy * uScale, 0.0, 1.0);
}
)";

constexpr const char* kSphereFragment = R"(#version 330 core
in vec2 vUV;
in vec3 vNormal;
uniform sampler2D uImage;
uniform vec3 uLight;
out vec4 FragColor;
void main() {
    vec3 n = normalize(vNormal);
    // A gentle terminator - enough to read as a ball, not so much that half the circuits sit
    // in the dark. The floor is deliberately high for that reason.
    float lambert = max(dot(n, normalize(uLight)), 0.0);
    float shade = 0.55 + 0.45 * lambert;
    // Limb darkening: the edge of the disc is the part turning away from us fastest.
    shade *= mix(0.65, 1.0, clamp(n.z * 1.6, 0.0, 1.0));
    FragColor = vec4(texture(uImage, vUV).rgb * shade, 1.0);
}
)";

constexpr const char* kMarkerVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColour;
out vec3 vColour;
void main() {
    vColour = aColour;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

constexpr const char* kMarkerFragment = R"(#version 330 core
in vec3 vColour;
out vec4 FragColor;
void main() { FragColor = vec4(vColour, 1.0); }
)";

// The waste colour of the gore maps: a flat green, painted between the orange-peel strips.
constexpr uint8_t kGoreIndex = 4;

// The two halves of a body's skin, side by side, decoded through their palette.
//
// ⚠️ THE GORE LAYOUT IS LEFT EXACTLY AS IT IS. The original's UVs pave the strips and never
// sample the green; cutting it out and stretching what was left is what the first version of
// this file did, and it is precisely what made the map wrong.
//
// The green IS repainted, and only the green: two passes of dilation push the map's own
// colours one texel out into the waste. Nothing a face samples changes - the face edges land
// on the strip boundary to the pixel - but it stops GL_LINEAR mixing a hair of green in along
// those edges as the globe turns.
std::vector<uint8_t> build_atlas(const Blob* left, const Blob* right,
                                 const std::vector<RGB>& palette) {
    if (!left || !right || palette.size() < 256) return {};
    if (left->size() != kTmSize || right->size() != kTmSize) return {};

    // The indices first, the two halves side by side.
    std::vector<uint8_t> idx(static_cast<size_t>(kAtlasWidth) * kAtlasHeight);
    for (int y = 0; y < kTmHeight; ++y)
        for (int half = 0; half < 2; ++half) {
            const Blob& src = half == 0 ? *left : *right;
            std::copy(src.begin() + static_cast<ptrdiff_t>(y) * kTmWidth,
                      src.begin() + static_cast<ptrdiff_t>(y + 1) * kTmWidth,
                      idx.begin() + static_cast<ptrdiff_t>(y) * kAtlasWidth + half * kTmWidth);
        }

    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<uint8_t> prev = idx;
        for (int y = 0; y < kAtlasHeight; ++y)
            for (int x = 0; x < kAtlasWidth; ++x) {
                const size_t o = static_cast<size_t>(y) * kAtlasWidth + x;
                if (prev[o] != kGoreIndex) continue;
                // A half never borrows from the other: they are two different sides of the
                // world and the join between them is a real edge, not a continuation.
                const int lo = x < kTmWidth ? 0 : kTmWidth;
                const int hi = lo + kTmWidth - 1;
                for (int dy = -1; dy <= 1 && idx[o] == kGoreIndex; ++dy)
                    for (int dx = -1; dx <= 1 && idx[o] == kGoreIndex; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < lo || nx > hi || ny < 0 || ny >= kAtlasHeight) continue;
                        const uint8_t n = prev[static_cast<size_t>(ny) * kAtlasWidth + nx];
                        if (n != kGoreIndex) idx[o] = n;
                    }
            }
    }

    std::vector<uint8_t> rgba(idx.size() * 4);
    for (size_t i = 0; i < idx.size(); ++i) {
        const RGB& c = palette[idx[i]];
        rgba[i * 4 + 0] = c.r;
        rgba[i * 4 + 1] = c.g;
        rgba[i * 4 + 2] = c.b;
        rgba[i * 4 + 3] = 255;
    }
    return rgba;
}

GLuint upload(const std::vector<uint8_t>& rgba, int w, int h) {
    if (rgba.empty() || w <= 0 || h <= 0) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    // LINEAR both ways: the gutter fill in build_atlas() means the edge of a strip blends
    // into more map, never into the waste green. CLAMP both ways too - the atlas is two
    // separate half-worlds and the poles are its own top and bottom edge; nothing wraps.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    glBindTexture(GL_TEXTURE_2D, 0);
    return id;
}

} // namespace

Globe::~Globe() { release(); }

void Globe::release() {
    for (GLuint& t : body_texture_)
        if (t) { glDeleteTextures(1, &t); t = 0; }
    if (vbo_) { gl::DeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { gl::DeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (marker_vbo_) { gl::DeleteBuffers(1, &marker_vbo_); marker_vbo_ = 0; }
    if (marker_vao_) { gl::DeleteVertexArrays(1, &marker_vao_); marker_vao_ = 0; }
    if (program_) { gl::DeleteProgram(program_); program_ = 0; }
    if (marker_program_) { gl::DeleteProgram(marker_program_); marker_program_ = 0; }
    names_[0].clear();
    names_[1].clear();
    screen_palette_.clear();
    globe_spr_.clear();
    vertex_count_ = 0;
}

// The globe's frame is the one the original builds, and `TOURN.TXT` speaks it already:
// longitude in the game's 1024 units is `globe_x / 64`, which in TURNS is just `globe_x` over
// a full 16-bit angle. A point at longitude L faces the viewer when the yaw IS L (see the
// vertex shader's note on the sign), so no calibration constant survives here - the one the
// first version carried was an artefact of the generic sphere it drew, not a fact about the
// game.
float Globe::yaw_for(int globe_x) {
    float t = static_cast<float>(globe_x) / 65536.0f;
    t -= std::floor(t);
    return t;
}

// `globe_y` is the colatitude, 0 at the north pole (formats/tourn.h). The latitude in turns is
// therefore a quarter turn minus it, and bringing that latitude to the middle of the disc means
// pitching by exactly it.
float Globe::pitch_for(int globe_y) {
    return 0.25f - static_cast<float>(globe_y) / 65536.0f;
}

bool Globe::build_program() {
    program_ = Shader::compile(kSphereVertex, kSphereFragment);
    if (!program_) return false;
    u_yaw_ = gl::GetUniformLocation(program_, "uYaw");
    u_pitch_ = gl::GetUniformLocation(program_, "uPitch");
    u_scale_ = gl::GetUniformLocation(program_, "uScale");
    u_centre_ = gl::GetUniformLocation(program_, "uCentre");
    u_tex_ = gl::GetUniformLocation(program_, "uImage");
    u_light_ = gl::GetUniformLocation(program_, "uLight");

    marker_program_ = Shader::compile(kMarkerVertex, kMarkerFragment);
    if (!marker_program_) return false;
    return true;
}

bool Globe::build_sphere() {
    struct V {
        float x, y, z, u, v;
    };
    // A point of the sphere from the two angles, in the game's own 1024-to-the-turn unit.
    //
    // ⚠️ THE VERTICAL IS NEGATED, as everywhere else in this project: the original's frame
    // runs the other way up (CLAUDE.md, "Conventions that bite"), so its band-0 pole - the one
    // it writes by hand as the literal `(0, 0, -radius)` - is the pole the texture wraps the
    // ARCTIC round, and the Arctic belongs at the top of the screen. Without this flip the
    // whole world is drawn upside down, which is exactly how it first came out.
    const auto point = [](int lon, int lat) {
        const float la = static_cast<float>(lat) * kTau / 1024.0f;
        const float lo = static_cast<float>(lon) * kTau / 1024.0f;
        const float ring = std::cos(la);
        return std::array<float, 3>{ring * std::sin(lo), -std::sin(la), ring * std::cos(lo)};
    };

    std::vector<V> verts;
    verts.reserve(static_cast<size_t>(kSlices) * kBands * 6);
    for (int slice = 0; slice < kSlices; ++slice) {
        // Which UV record and which half of the atlas. Both come straight from the original:
        // `5 - j % 6` for the record, `j < 6` for the texture.
        const int record = 5 - (slice % 6);
        const float u_base = static_cast<float>(record * kUvSlicePitch +
                                                (slice < 6 ? 0 : kTmWidth));
        const int lon_a = ((slice + 1) % kSlices) * kSliceAngle + kLonPhase;
        const int lon_b = ((slice + 2) % kSlices) * kSliceAngle + kLonPhase;

        for (int band = 0; band < kBands; ++band) {
            const int ring_p = kFirstRing + kRingStep * band;
            const int ring_q = ring_p + kRingStep;
            const auto uv = [&](int corner) {
                return std::array<float, 2>{
                    (u_base + static_cast<float>(kSliceUv[band][corner].u)) / kAtlasWidth,
                    static_cast<float>(kSliceUv[band][corner].v) / kAtlasHeight};
            };
            const auto emit = [&](const std::array<float, 3>& p, int corner) {
                const std::array<float, 2> t = uv(corner);
                verts.push_back(V{p[0], p[1], p[2], t[0], t[1]});
            };

            // ⚠️ EVERY FACE IS EMITTED BACK TO FRONT of the order the original lists it in.
            // Negating the vertical above mirrors the sphere, which reverses the winding of
            // all 96 faces; back-face culling is this screen's whole hidden-surface removal,
            // so leaving them as listed would keep the FAR half and throw the near one away.
            if (band == 0) {
                // The cap at the top: the pole, then the first ring. The original writes this
                // vertex as a literal (0, 0, -radius) - here (0, +1, 0) after the flip.
                const std::array<float, 3> pole{0.0f, 1.0f, 0.0f};
                const std::array<float, 3> a2 = point(lon_a, ring_q);
                const std::array<float, 3> b2 = point(lon_b, ring_q);
                emit(a2, 1); emit(b2, 2); emit(pole, 0);
            } else if (band == kBands - 1) {
                const std::array<float, 3> pole{0.0f, -1.0f, 0.0f};
                const std::array<float, 3> a2 = point(lon_b, ring_p);
                const std::array<float, 3> b2 = point(lon_a, ring_p);
                emit(b2, 1); emit(pole, 2); emit(a2, 0);
            } else {
                // 0 and 1 sit on ring Q, 2 and 3 on ring P - the order the UV table is in.
                const std::array<float, 3> p0 = point(lon_a, ring_q);
                const std::array<float, 3> p1 = point(lon_b, ring_q);
                const std::array<float, 3> p2 = point(lon_b, ring_p);
                const std::array<float, 3> p3 = point(lon_a, ring_p);
                emit(p2, 2); emit(p3, 3); emit(p0, 0);
                emit(p1, 1); emit(p2, 2); emit(p0, 0);
            }
        }
    }
    vertex_count_ = static_cast<int>(verts.size());

    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);
    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(GL_ARRAY_BUFFER_, vbo_);
    gl::BufferData(GL_ARRAY_BUFFER_, static_cast<GLsizeiptr>(verts.size() * sizeof(V)),
                   verts.data(), GL_STATIC_DRAW_);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), reinterpret_cast<void*>(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V),
                            reinterpret_cast<void*>(sizeof(float) * 3));

    gl::GenVertexArrays(1, &marker_vao_);
    gl::BindVertexArray(marker_vao_);
    gl::GenBuffers(1, &marker_vbo_);
    gl::BindBuffer(GL_ARRAY_BUFFER_, marker_vbo_);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5,
                            reinterpret_cast<void*>(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5,
                            reinterpret_cast<void*>(sizeof(float) * 2));
    gl::BindVertexArray(0);
    return true;
}

bool Globe::load(const std::string& game_dir) {
    release();
    ArchivePtr scn_archive;
    try {
        scn_archive = game_archive(game_dir, "GLOBESCN.SCN");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Globe: GLOBESCN.SCN did not open (%s)\n", e.what());
        return false;
    }
    const Archive& scn = *scn_archive;
    const auto member = [&scn](const char* name) -> const Blob* {
        const auto it = scn.find(name);
        return it == scn.end() ? nullptr : &it->second;
    };

    if (!build_program()) { release(); return false; }
    if (!build_sphere()) { release(); return false; }

    // The Earth: two tiles, the screen's own `GLOBE.COL`.
    if (const Blob* col = member("GLOBE.COL")) {
        const std::vector<RGB> palette = load_palette_rgb(col->data(), col->size());
        const std::vector<uint8_t> rgba =
            build_atlas(member("WORLDTX1.TM"), member("WORLDTX2.TM"), palette);
        body_texture_[static_cast<int>(GlobeBody::Earth)] =
            upload(rgba, kAtlasWidth, kAtlasHeight);
    }
    // ⚠️ MARS, VENUS AND THE MOON ARE ONE TEXTURE AND THREE PALETTES. `VENUSTEX.TM` is the only
    // planet artwork in the archive and `MARSPAL`/`VENUSPAL`/`MOONPAL.COL` sit beside it: the
    // three worlds differ in colour alone, which is why the file is named after just one.
    static const struct { GlobeBody body; const char* pal; } kPlanets[] = {
        {GlobeBody::Mars, "MARSPAL.COL"},
        {GlobeBody::Venus, "VENUSPAL.COL"},
        {GlobeBody::Moon, "MOONPAL.COL"},
    };
    for (const auto& p : kPlanets) {
        const Blob* col = member(p.pal);
        if (!col) continue;
        const std::vector<RGB> palette = load_palette_rgb(col->data(), col->size());
        // All twelve slices take the one texture, so both halves of the atlas are it.
        const Blob* tex = member("VENUSTEX.TM");
        const std::vector<uint8_t> rgba = build_atlas(tex, tex, palette);
        body_texture_[static_cast<int>(p.body)] = upload(rgba, kAtlasWidth, kAtlasHeight);
    }

    if (const Blob* n = member("NAMES.TM")) names_[0] = *n;
    if (const Blob* n = member("NAMES2.TM")) names_[1] = *n;
    // `GLOBESCN.SPR` carries no palette of its own; it is drawn over `THIRD3.RAW`, so that
    // background's own first 768 bytes are its palette. See formats/sprite.h.
    if (const Blob* raw = member("THIRD3.RAW"); raw && raw->size() >= kRawPaletteBytes)
        screen_palette_.assign(raw->begin(), raw->begin() + kRawPaletteBytes);
    if (const Blob* s = member("GLOBESCN.SPR")) globe_spr_ = *s;

    return ready();
}

std::vector<uint8_t> Globe::name_image(int name_index) const {
    // The sheet is laid out COLUMN-MAJOR in 128x32 cells: six names down the left column of
    // `NAMES.TM`, six down its right, then the same again in `NAMES2.TM` - which is what makes
    // all 24 circuits land on their own country. See formats/tourn.h.
    constexpr int kRows = kTmHeight / kNameHeight;          // 6
    constexpr int kPerSheet = kRows * (kTmWidth / kNameWidth); // 12
    if (name_index < 0 || name_index >= kPerSheet * 2) return {};
    const int sheet = name_index / kPerSheet;
    if (names_[sheet].size() != kTmSize || screen_palette_.empty()) return {};
    const int cell = name_index % kPerSheet;
    const int col = cell / kRows, row = cell % kRows;

    const std::vector<RGB> palette = load_palette_rgb(screen_palette_.data(),
                                                      screen_palette_.size());
    std::vector<uint8_t> rgba(static_cast<size_t>(kNameWidth) * kNameHeight * 4, 0);
    for (int y = 0; y < kNameHeight; ++y) {
        for (int x = 0; x < kNameWidth; ++x) {
            const uint8_t index =
                names_[sheet][static_cast<size_t>(row * kNameHeight + y) * kTmWidth +
                              static_cast<size_t>(col * kNameWidth + x)];
            if (index == 0) continue;
            const size_t o = (static_cast<size_t>(y) * kNameWidth + x) * 4;
            const RGB& c = palette[index];
            rgba[o + 0] = c.r;
            rgba[o + 1] = c.g;
            rgba[o + 2] = c.b;
            rgba[o + 3] = 255;
        }
    }
    return rgba;
}

std::vector<uint8_t> Globe::widget_image(Widget widget, int frame, int& out_w,
                                         int& out_h) const {
    out_w = out_h = 0;
    if (globe_spr_.empty() || screen_palette_.empty()) return {};
    // `GLOBESCN.SPR` holds fifteen glyphs: 0-4 the right arrow's five frames, 5-9 the left
    // arrow's, 10-14 the passport stamp's. The script's own `anisprite` lines name exactly
    // those three roots (0, 5 and 10).
    const int base = widget == Widget::RightArrow ? 0 : (widget == Widget::LeftArrow ? 5 : 10);
    const std::vector<SpriteGlyph> glyphs = decode_spr(globe_spr_.data(), globe_spr_.size());
    const size_t index = static_cast<size_t>(base + (frame % 5 + 5) % 5);
    if (index >= glyphs.size()) return {};
    const std::vector<RGB> palette = load_palette_rgb(screen_palette_.data(),
                                                      screen_palette_.size());
    out_w = glyphs[index].width;
    out_h = glyphs[index].height;
    return sprite_glyph_rgba(glyphs[index], palette);
}

void Globe::draw(int canvas_w, int canvas_h, float cx, float cy, float radius, float yaw_turns,
                 float pitch_turns, GlobeBody body, const std::vector<GlobeMarker>& markers) {
    if (!ready() || canvas_w <= 0 || canvas_h <= 0) return;
    const GLuint texture = body_texture_[static_cast<int>(body)];
    if (!texture) return;

    // The caller lays out in canvas pixels from the TOP-LEFT, like every document on this
    // screen does (ui/rml_render_interface.cpp); GL wants -1..1 from the bottom-left.
    const float ndc_cx = cx / static_cast<float>(canvas_w) * 2.0f - 1.0f;
    const float ndc_cy = 1.0f - cy / static_cast<float>(canvas_h) * 2.0f;
    const float sx = radius / static_cast<float>(canvas_w) * 2.0f;
    const float sy = radius / static_cast<float>(canvas_h) * 2.0f;
    const float yaw = yaw_turns * kTau;
    const float pitch = pitch_turns * kTau;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);   // the shader mirrors X, which turns every triangle round
    gl::UseProgram(program_);
    if (u_yaw_ >= 0) gl::Uniform1f(u_yaw_, yaw);
    if (u_pitch_ >= 0) gl::Uniform1f(u_pitch_, pitch);
    if (u_scale_ >= 0) gl::Uniform2f(u_scale_, sx, sy);
    if (u_centre_ >= 0) gl::Uniform2f(u_centre_, ndc_cx, ndc_cy);
    if (u_light_ >= 0) gl::Uniform3f(u_light_, -0.35f, 0.45f, 0.82f);
    gl::ActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D, texture);
    if (u_tex_ >= 0) gl::Uniform1i(u_tex_, 0);
    gl::BindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, vertex_count_);

    // --- the markers -------------------------------------------------------------------
    // ⚠️ HIDDEN BY HAND, because the canvas has no depth buffer: a marker on the far side of
    // the globe would otherwise be drawn straight over the ocean in front of it.
    std::vector<float> quad;
    quad.reserve(markers.size() * 6 * 5);
    for (const GlobeMarker& m : markers) {
        const float lat = (0.5f - static_cast<float>(m.globe_y) / 65536.0f * 2.0f) * kPi;
        const float lon = static_cast<float>(m.globe_x) / 65536.0f * kTau;
        // The same two rotations the vertex shader does, on one point.
        float px = std::cos(lat) * std::sin(lon);
        const float py0 = std::sin(lat);
        const float pz0 = std::cos(lat) * std::cos(lon);
        const float cyaw = std::cos(yaw), syaw = std::sin(yaw);
        const float x1 = px * cyaw - pz0 * syaw;
        const float z1 = px * syaw + pz0 * cyaw;
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        const float y2 = py0 * cp - z1 * sp;
        const float z2 = py0 * sp + z1 * cp;
        if (z2 <= 0.08f) continue;   // the far side, or grazing the limb
        // Lifted just off the surface so it is never swallowed by the sphere's own curvature.
        const float ox = ndc_cx - x1 * 1.01f * sx;   // mirrored, like the sphere (shader)
        const float oy = ndc_cy + y2 * 1.01f * sy;
        const float half = (m.selected ? 0.075f : 0.045f) * radius;
        const float hx = half / static_cast<float>(canvas_w) * 2.0f;
        const float hy = half / static_cast<float>(canvas_h) * 2.0f;
        const float r = m.selected ? 1.0f : 0.85f;
        const float g = m.selected ? 0.87f : 0.15f;
        const float b = m.selected ? 0.25f : 0.15f;
        // A diamond, which reads at this size where a square reads as a dropped pixel.
        const float pts[4][2] = {{0, hy}, {hx, 0}, {0, -hy}, {-hx, 0}};
        const int tri[6] = {0, 1, 2, 0, 2, 3};
        for (const int i : tri) {
            quad.push_back(ox + pts[i][0]);
            quad.push_back(oy + pts[i][1]);
            quad.push_back(r);
            quad.push_back(g);
            quad.push_back(b);
        }
    }
    if (!quad.empty()) {
        gl::UseProgram(marker_program_);
        gl::BindVertexArray(marker_vao_);
        gl::BindBuffer(GL_ARRAY_BUFFER_, marker_vbo_);
        gl::BufferData(GL_ARRAY_BUFFER_, static_cast<GLsizeiptr>(quad.size() * sizeof(float)),
                       quad.data(), GL_STREAM_DRAW_);   // rebuilt every frame
        glDisable(GL_CULL_FACE);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(quad.size() / 5));
    }

    gl::BindVertexArray(0);
    gl::UseProgram(0);
    glDisable(GL_CULL_FACE);
}

} // namespace bigred
