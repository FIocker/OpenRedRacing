#include "vehicle_display.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "formats/terrain.h"        // TerrainVertex - what place_vehicle emits
#include "formats/texture.h"        // decode_tm_rgba, kTmWidth/kTmHeight
#include "formats/vehicle_script.h" // load_vehicle
#include "scene/character_model.h"
#include "render/mat4.h"
#include "render/shader.h"

namespace bigred {
namespace {

// A vehicle's bodywork is never distance-shaded: its face colours ALREADY are a baked luminance
// ramp of the team colour. So this is not the world's shader,
// which would walk those colours up their ramp and erase the very thing they encode - it is a
// matrix, a colour, and one array texture for the pieces that really are textured (wheel rims,
// hubcaps: see scene/vehicle_render.cpp's is_texture_path_face).
//
// `aTex` is `u, v, layer, has_texture` - four consecutive floats of TerrainVertex, so they come
// across as one attribute and nothing has to be repacked.
constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec4 aTex;
uniform mat4 uMVP;
out vec3 vColor;
out vec4 vTex;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
    vTex = aTex;
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec3 vColor;
in vec4 vTex;
uniform sampler2DArray uAtlas;
out vec4 FragColor;
void main() {
    if (vTex.w > 0.5) {
        vec4 texel = texture(uAtlas, vec3(vTex.xy, vTex.z));
        // Palette index 0 decodes transparent, and a rim's sheet uses it - see
        // formats/texture.h. Dropped rather than blended: there is no sorted transparency here.
        if (texel.a < 0.5) discard;
        FragColor = vec4(texel.rgb, 1.0);
    } else {
        FragColor = vec4(vColor, 1.0);
    }
}
)";

constexpr float kTau = 6.283185307179586f;

// The camera's vertical field of view. Narrow, so the perspective stays gentle: a menu shows a
// product, and a wide lens on a small car reads as a fish-eye.
constexpr float kFovY = 40.0f * kTau / 360.0f;

// A slight look-down, on top of the vehicle's own forward lean. It is the camera's, not the
// model's, so it does NOT turn with the car - which is what keeps the roof visible all the way
// round instead of only when the bonnet faces the viewer.
constexpr float kEyeRise = 0.20f;

} // namespace

VehicleDisplay::~VehicleDisplay() { release(); }

void VehicleDisplay::release() {
    if (textures_) { glDeleteTextures(1, &textures_); textures_ = 0; }
    texture_count_ = 0;
    if (vbo_) { gl::DeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { gl::DeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (program_) { gl::DeleteProgram(program_); program_ = 0; }
    model_ = VehicleModel{};
}

// The collected sheets as the layers of one GL_TEXTURE_2D_ARRAY, the same shape the world's
// atlas has. An empty set is normal - most vehicles' bodywork is pure flat colour - and then
// nothing binds and every face takes the colour branch.
void VehicleDisplay::upload_textures(const std::vector<std::vector<uint8_t>>& sheets) {
    if (textures_) { glDeleteTextures(1, &textures_); textures_ = 0; }
    texture_count_ = 0;
    if (sheets.empty()) return;
    // One contiguous upload, layers back to back - the same way the world builds its atlas,
    // and it needs only `TexImage3D`. A sheet that decoded to the wrong size is passed over as
    // a transparent layer rather than shifting every layer after it.
    constexpr size_t kLayerBytes = static_cast<size_t>(kTmWidth) * kTmHeight * 4;
    std::vector<uint8_t> stacked(kLayerBytes * sheets.size(), 0);
    for (size_t i = 0; i < sheets.size(); ++i) {
        if (sheets[i].size() != kLayerBytes) continue;
        std::copy(sheets[i].begin(), sheets[i].end(), stacked.begin() + kLayerBytes * i);
    }
    glGenTextures(1, &textures_);
    glBindTexture(GL_TEXTURE_2D_ARRAY_, textures_);
    gl::TexImage3D(GL_TEXTURE_2D_ARRAY_, 0, GL_RGBA, kTmWidth, kTmHeight,
                   static_cast<GLsizei>(sheets.size()), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   stacked.data());
    // NEAREST, like every other piece of this game's artwork: the sheets are 1996 pixel art and
    // smoothing a hubcap would make it look like a different game.
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    glBindTexture(GL_TEXTURE_2D_ARRAY_, 0);
    texture_count_ = static_cast<int>(sheets.size());
}

bool VehicleDisplay::build_gl() {
    if (program_) return true;
    program_ = Shader::compile(kVertexShader, kFragmentShader);
    if (!program_) {
        std::fprintf(stderr, "VehicleDisplay: shader failed to compile.\n");
        return false;
    }
    u_mvp_ = gl::GetUniformLocation(program_, "uMVP");
    u_atlas_ = gl::GetUniformLocation(program_, "uAtlas");
    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);
    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(GL_ARRAY_BUFFER_, vbo_);
    // The stride is the whole TerrainVertex: the UV, the atlas layer and the bright end of the
    // ramp ride along unread rather than being copied out into a tighter struct, which would be
    // a second definition of what a vehicle vertex is.
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex),
                            reinterpret_cast<void*>(offsetof(TerrainVertex, x)));
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex),
                            reinterpret_cast<void*>(offsetof(TerrainVertex, r)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex),
                            reinterpret_cast<void*>(offsetof(TerrainVertex, u)));
    gl::EnableVertexAttribArray(2);
    gl::BindVertexArray(0);
    return true;
}

bool VehicleDisplay::load(const std::string& game_dir, const std::string& name,
                          const std::vector<RGB>& palette, const VehicleTint* tint,
                          const VehicleLogo* logo, const VehicleDriver* driver) {
    model_ = VehicleModel{};
    if (palette.empty()) return false;

    VehicleTuning tune;
    ArchivePtr vhc;
    if (!load_vehicle(game_dir, name, tune, &vhc)) {
        std::fprintf(stderr, "VehicleDisplay: could not load %s.VHC\n", name.c_str());
        return false;
    }
    // The vehicle's own sheets, collected while the model is built and stacked into one array
    // texture afterwards. The sink is what keeps the geometry code identical to the race's: the
    // UVs and the layer numbers are worked out in exactly one place, only the storage differs.
    // ⚠️ THE SHEET ARRIVES DECODED, and it matters that this signature says so: `Blob` is a
    // byte vector like any other, so a sink written for raw `.TM` bytes would still compile
    // here and would quietly decode already-decoded pixels - leaving the menu with no textures
    // at all and no error anywhere.
    std::vector<std::vector<uint8_t>> sheets;
    model_ = build_vehicle_model(*vhc, tune, 1, palette, tint,
                                 [&sheets](std::vector<uint8_t> rgba) -> int {
                                     if (rgba.empty()) return -1;
                                     sheets.push_back(std::move(rgba));
                                     return static_cast<int>(sheets.size()) - 1;
                                 },
                                 logo, driver);
    tilt_turns_ = kForwardTiltTurns;
    return finish(sheets);
}

bool VehicleDisplay::load_character(const std::string& game_dir, int index,
                                    const std::vector<RGB>& palette, const VehicleTint* tint) {
    model_ = VehicleModel{};
    if (palette.empty()) return false;
    CharacterShapes shapes;
    if (!load_character_shapes(game_dir, index, shapes)) {
        std::fprintf(stderr, "VehicleDisplay: could not load character %d\n", index);
        return false;
    }
    std::vector<std::vector<uint8_t>> sheets;
    int mouth_layer = -1;
    if (shapes.mouth_tm.size() == kTmSize) {
        sheets.push_back(decode_tm_rgba(shapes.mouth_tm.data(), palette));
        mouth_layer = 0;
    }
    model_.body = shape_mesh(shapes.body, palette, tint, -1);
    VehiclePart head;
    head.local = shape_mesh(shapes.head, palette, tint, mouth_layer);
    // Game axes, +Y down: the head's origin is `head_rise` ABOVE the body's.
    head.pos[1] = static_cast<int32_t>(-shapes.head_rise);
    head.flags = 0xFF;
    if (!head.local.empty()) model_.parts.push_back(std::move(head));
    tilt_turns_ = 0.0f;
    return finish(sheets);
}

bool VehicleDisplay::finish(const std::vector<std::vector<uint8_t>>& sheets) {
    if (!model_.valid()) return false;
    if (!build_gl()) return false;
    upload_textures(sheets);

    // What the camera has to frame. Measured on the vehicle AT REST and about its own origin -
    // which is the axis it turns and leans about, so the sphere is the same one whatever the
    // pose, and the car never swells or shrinks as it comes round.
    const std::vector<TerrainVertex> rest = place_vehicle(model_, VehiclePose{}, 1, 1.0f);
    if (rest.empty()) return false;
    float min_y = rest[0].y, max_y = rest[0].y;
    for (const TerrainVertex& v : rest) {
        min_y = std::min(min_y, v.y);
        max_y = std::max(max_y, v.y);
    }
    centre_y_ = (min_y + max_y) * 0.5f;
    radius_ = 1.0f;
    for (const TerrainVertex& v : rest) {
        const float dy = v.y - centre_y_;
        radius_ = std::max(radius_, std::sqrt(v.x * v.x + dy * dy + v.z * v.z));
    }
    return true;
}

void VehicleDisplay::draw(int canvas_w, int canvas_h, float x, float y, float w, float h,
                          float turn) {
    if (!ready() || canvas_w <= 0 || canvas_h <= 0 || w < 1.0f || h < 1.0f) return;

    // ⚠️ THE POSE IS THE SIMULATION'S OWN, and that is the whole trick: `place_vehicle` already
    // knows how a body's pitch and yaw turn it, where each wheel hangs and which way it points.
    // Handing it a pose with a constant pitch and a turning yaw is all "the vehicle stands
    // nose-down and revolves" needs. The pitch is applied in the VEHICLE's frame, before the
    // yaw, so the nose stays the part that dips: the car never rolls onto its side halfway
    // round, which is what a tilt applied after the yaw would do.
    VehiclePose pose;
    pose.pitch = tilt_turns_ * 65536.0f;
    pose.yaw = turn * 65536.0f;
    const std::vector<TerrainVertex> mesh = place_vehicle(model_, pose, 1, 1.0f);
    if (mesh.empty()) return;

    // The canvas lays out from the TOP-LEFT (ui/rml_render_interface.cpp); GL counts its
    // viewport from the bottom-left.
    const GLint vx = static_cast<GLint>(x);
    const GLint vy = static_cast<GLint>(static_cast<float>(canvas_h) - (y + h));
    const GLsizei vw = static_cast<GLsizei>(w);
    const GLsizei vh = static_cast<GLsizei>(h);
    const float aspect = w / h;

    // Far enough back that the bounding sphere fits the NARROWER of the two fields of view: a
    // tall, narrow box is bound by the horizontal one, and framing on the vertical alone would
    // cut the car's nose off as it turns side-on.
    const float half_v = kFovY * 0.5f;
    const float half_h = std::atan(std::tan(half_v) * aspect);
    const float half = std::min(half_v, half_h);
    const float distance = radius_ / std::max(0.05f, std::sin(half)) * 1.05f;

    const Mat4::Vec3 target{0.0f, centre_y_, 0.0f};
    const Mat4::Vec3 eye{0.0f, centre_y_ + distance * kEyeRise, distance};
    const Mat4 mvp = Mat4::perspective(kFovY, aspect, radius_ * 0.05f, distance + radius_ * 4.0f) *
                     Mat4::lookAt(eye, target, {0.0f, 1.0f, 0.0f});

    glViewport(vx, vy, vw, vh);
    // ⚠️ A VEHICLE IS BACK-FACE CULLED, and that is what makes its glass read as glass. The
    // original draws only the INSIDE of the canopy and windscreen polygons, so from outside you
    // look straight through into the cabin - no alpha anywhere, just a face that is never drawn
    // from the front. Described by the user, 2026-09-23. Without culling both sides are drawn
    // and the glass comes out as a solid bright blue panel.
    //
    // ⚠️ GL_FRONT, not GL_BACK: `place_vehicle` mirrors world X (the world itself is mirrored -
    // see scene/vehicle_render.h), which reverses every triangle's winding on the way out.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    gl::UseProgram(program_);
    if (u_mvp_ >= 0) gl::UniformMatrix4fv(u_mvp_, 1, GL_FALSE, &mvp.m[0][0]);
    if (texture_count_ > 0) {
        gl::ActiveTexture(GL_TEXTURE0_);
        glBindTexture(GL_TEXTURE_2D_ARRAY_, textures_);
        if (u_atlas_ >= 0) gl::Uniform1i(u_atlas_, 0);
    }
    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER_, vbo_);
    // Re-uploaded every frame, like the globe's markers and like the race's own vehicle: a car
    // is a few thousand triangles, and the alternative is a second static copy of it with the
    // pose baked in - i.e. a matrix stack this does not otherwise need.
    gl::BufferData(GL_ARRAY_BUFFER_,
                   static_cast<GLsizeiptr>(mesh.size() * sizeof(TerrainVertex)), mesh.data(),
                   GL_STREAM_DRAW_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh.size()));
    gl::BindVertexArray(0);
    gl::UseProgram(0);

    // ⚠️ PUT BACK WHAT THE DOCUMENT EXPECTS. RmlUi's render interface is handed the canvas'
    // size and draws against the CURRENT viewport (ui/rml_render_interface.cpp) - leaving this
    // one set would squeeze the whole interface into the little box the car sits in.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, canvas_w, canvas_h);
}

} // namespace bigred
