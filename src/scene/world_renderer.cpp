#include "world_renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>

#include "formats/texture.h"   // kTmWidth/kTmHeight, the atlas layer dimensions
#include "render/shader.h"
#include "render/world_shaders.h"

namespace bigred {
namespace {

// The one vertex layout the whole engine draws with (formats/terrain.h's TerrainVertex).
void bind_attribs() {
    using V = TerrainVertex;
    struct Attr {
        GLuint index;
        GLint size;
        size_t offset;
    };
    static const Attr kAttrs[] = {
        {0, 3, 0},
        {1, 3, offsetof(V, r)},
        {2, 2, offsetof(V, u)},
        {3, 1, offsetof(V, layer)},
        {4, 1, offsetof(V, has_texture)},
        {5, 3, offsetof(V, br)},
        {6, 1, offsetof(V, lit)},
        {7, 1, offsetof(V, cull)},
    };
    for (const Attr& a : kAttrs) {
        gl::VertexAttribPointer(a.index, a.size, GL_FLOAT, GL_FALSE, sizeof(V),
                                reinterpret_cast<void*>(a.offset));
        gl::EnableVertexAttribArray(a.index);
    }
}

void make_buffer(GLuint& vao, GLuint& vbo) {
    gl::GenVertexArrays(1, &vao);
    gl::BindVertexArray(vao);
    gl::GenBuffers(1, &vbo);
    gl::BindBuffer(GL_ARRAY_BUFFER_, vbo);
    bind_attribs();
    gl::BindVertexArray(0);
}

void fill(GLuint vbo, const std::vector<TerrainVertex>& v, GLenum usage) {
    gl::BindBuffer(GL_ARRAY_BUFFER_, vbo);
    gl::BufferData(GL_ARRAY_BUFFER_, static_cast<GLsizeiptr>(v.size() * sizeof(TerrainVertex)),
                   v.empty() ? nullptr : v.data(), usage);
}

// Everything that is decor rather than ground takes the same depth bias toward the camera. A
// decal lying flat on the ground - a helipad, a start line - sits at exactly the terrain height
// and z-fights with it; the original paints into a depth-sorted queue and never has the
// problem. glPolygonOffset is the depth-buffer equivalent, and unlike a fixed lift it scales
// with distance and slope.
struct DecorBias {
    DecorBias() {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
    }
    ~DecorBias() { glDisable(GL_POLYGON_OFFSET_FILL); }
};

} // namespace

WorldRenderer::~WorldRenderer() {
    // ⚠️ Not release(): a destructor cannot promise a current GL context. The owner calls
    // release() from its exit() - see the header.
}

bool WorldRenderer::create() {
    if (program_) return true;
    program_ = Shader::compile(kVertexShader, kFragmentShader);
    if (!program_) {
        std::fprintf(stderr, "World shader failed to compile.\n");
        return false;
    }
    mvp_loc_ = gl::GetUniformLocation(program_, "uMVP");
    atlas_loc_ = gl::GetUniformLocation(program_, "uAtlas");
    cam_pos_loc_ = gl::GetUniformLocation(program_, "uCamPos");
    shade_raw_loc_ = gl::GetUniformLocation(program_, "uShadeRaw");
    shade_span_loc_ = gl::GetUniformLocation(program_, "uShadeSpan");
    atlas_layers_loc_ = gl::GetUniformLocation(program_, "uAtlasLayers");
    shade_strength_loc_ = gl::GetUniformLocation(program_, "uShadeStrength");
    shade_dark_ceil_loc_ = gl::GetUniformLocation(program_, "uShadeDarkCeil");
    cull_side_loc_ = gl::GetUniformLocation(program_, "uCullSide");

    // The backdrop: its own program and an empty VAO - the fullscreen triangle comes from
    // gl_VertexID, but GL 3.3 core refuses to draw with no VAO bound at all.
    sky_program_ = Shader::compile(kSkyVertexShader, kSkyFragmentShader);
    if (!sky_program_)
        std::fprintf(stderr, "[sky] shader failed to compile: flat clear colour instead.\n");
    gl::GenVertexArrays(1, &sky_vao_);
    if (sky_program_) {
        sky_u_.forward = gl::GetUniformLocation(sky_program_, "uForward");
        sky_u_.right = gl::GetUniformLocation(sky_program_, "uRight");
        sky_u_.up = gl::GetUniformLocation(sky_program_, "uUp");
        sky_u_.tan_half = gl::GetUniformLocation(sky_program_, "uTanHalfFov");
        sky_u_.aspect = gl::GetUniformLocation(sky_program_, "uAspect");
        for (size_t i = 0; i < kSkyBands.size(); ++i) {
            sky_u_.sky_color[i] = gl::GetUniformLocation(
                sky_program_, ("uSkyColor[" + std::to_string(i) + "]").c_str());
            sky_u_.sky_angle[i] = gl::GetUniformLocation(
                sky_program_, ("uSkyAngle[" + std::to_string(i) + "]").c_str());
        }
        for (size_t i = 0; i < kGroundBands.size(); ++i) {
            sky_u_.ground_color[i] = gl::GetUniformLocation(
                sky_program_, ("uGroundColor[" + std::to_string(i) + "]").c_str());
            sky_u_.ground_angle[i] = gl::GetUniformLocation(
                sky_program_, ("uGroundAngle[" + std::to_string(i) + "]").c_str());
        }
    }
    for (Buffer* b : {&world_, &anim_, &wreck_, &lights_, &sprites_, &horizon_, &vehicles_,
                      &effects_, &lines_, &cabin_, &cabin_parts_})
        make_buffer(b->vao, b->vbo);
    return true;
}

void WorldRenderer::upload_meshes(const LevelScene& scene) {
    fill(world_.vbo, scene.vbuf, GL_STATIC_DRAW_);
    vbuf_size_ = scene.vbuf.size();
    terrain_vertices_ = std::min(scene.vbuf_terrain_vertices, scene.vbuf.size());
    // Every step of every `anim` slot in one buffer: changing step is a different draw RANGE,
    // never any buffer traffic.
    fill(anim_.vbo, scene.anim_packed(&anim_ranges_), GL_STATIC_DRAW_);
    // Every state of every destructible, baked once: a cone going down is a different range.
    fill(wreck_.vbo, scene.wreck_buf, GL_STATIC_DRAW_);
    fill(lights_.vbo, scene.lights_buf, GL_STATIC_DRAW_);
}

void WorldRenderer::upload(const LevelScene& scene) {
    upload_meshes(scene);
    if (atlas_) {
        glDeleteTextures(1, &atlas_);
        atlas_ = 0;
    }
    atlas_layers_ = scene.atlas_count;
    if (scene.atlas_count <= 0) return;
    // One array, two halves: the raw decode, then the same images through the brightened
    // palette. The fragment shader samples both and mixes by distance (uAtlasLayers). If the
    // halves ever differ in size, the raw one goes in twice rather than a short buffer.
    std::vector<uint8_t> both = scene.atlas_rgb;
    const std::vector<uint8_t>& lit = (scene.atlas_rgb_bright.size() == scene.atlas_rgb.size())
                                          ? scene.atlas_rgb_bright
                                          : scene.atlas_rgb;
    both.insert(both.end(), lit.begin(), lit.end());
    glGenTextures(1, &atlas_);
    glBindTexture(GL_TEXTURE_2D_ARRAY_, atlas_);
    gl::TexImage3D(GL_TEXTURE_2D_ARRAY_, 0, GL_RGBA, kTmWidth, kTmHeight, scene.atlas_count * 2,
                   0, GL_RGBA, GL_UNSIGNED_BYTE, both.data());
    const GLint filter = smooth_textures_ ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    glBindTexture(GL_TEXTURE_2D_ARRAY_, 0);
}

void WorldRenderer::set_smooth_textures(bool smooth) {
    if (smooth == smooth_textures_) return;
    smooth_textures_ = smooth;
    if (!atlas_) return;   // upload() applies it
    const GLint filter = smooth ? GL_LINEAR : GL_NEAREST;
    glBindTexture(GL_TEXTURE_2D_ARRAY_, atlas_);
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_MAG_FILTER, filter);
    glBindTexture(GL_TEXTURE_2D_ARRAY_, 0);
}

void WorldRenderer::update_layer(int layer, const std::vector<uint8_t>& rgba) {
    if (!atlas_ || layer < 0 || layer >= atlas_layers_ ||
        rgba.size() != static_cast<size_t>(kTmWidth) * kTmHeight * 4)
        return;
    glBindTexture(GL_TEXTURE_2D_ARRAY_, atlas_);
    for (const int z : {layer, layer + atlas_layers_})
        gl::TexSubImage3D(GL_TEXTURE_2D_ARRAY_, 0, 0, 0, z, kTmWidth, kTmHeight, 1, GL_RGBA,
                          GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D_ARRAY_, 0);
}

void WorldRenderer::release() {
    if (atlas_) glDeleteTextures(1, &atlas_);
    atlas_ = 0;
    for (Buffer* b : {&world_, &anim_, &wreck_, &lights_, &sprites_, &horizon_, &vehicles_,
                      &effects_, &lines_, &cabin_, &cabin_parts_}) {
        if (b->vao) gl::DeleteVertexArrays(1, &b->vao);
        if (b->vbo) gl::DeleteBuffers(1, &b->vbo);
        *b = Buffer();
    }
    if (sky_vao_) gl::DeleteVertexArrays(1, &sky_vao_);
    sky_vao_ = 0;
    if (program_) gl::DeleteProgram(program_);
    if (sky_program_) gl::DeleteProgram(sky_program_);
    program_ = sky_program_ = 0;
    anim_ranges_.clear();
    vbuf_size_ = terrain_vertices_ = 0;
}

void WorldRenderer::stream(Buffer& b, const std::vector<TerrainVertex>& v, GLenum mode) {
    if (v.empty()) return;
    gl::BindVertexArray(b.vao);
    fill(b.vbo, v, GL_STREAM_DRAW_);
    glDrawArrays(mode, 0, static_cast<GLsizei>(v.size()));
}

void WorldRenderer::draw_sky(const LevelScene& scene, const WorldView& v) {
    if (!sky_program_ || scene.palette.empty()) return;
    // Depth TEST off, not just depth writes: the fullscreen triangle sits at NDC z = 1.0,
    // exactly the cleared value, and GL_LESS rejects "equal" - which looks exactly like the
    // pass never ran.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    gl::UseProgram(sky_program_);
    gl::Uniform3f(sky_u_.forward, v.forward.x, v.forward.y, v.forward.z);
    gl::Uniform3f(sky_u_.right, v.right.x, v.right.y, v.right.z);
    gl::Uniform3f(sky_u_.up, v.up.x, v.up.y, v.up.z);
    gl::Uniform1f(sky_u_.tan_half, v.tan_half_fov);
    gl::Uniform1f(sky_u_.aspect, v.aspect);
    for (size_t i = 0; i < kSkyBands.size(); ++i) {
        float rgb[3];
        sky_band_rgb(scene.palette, kSkyBands[i].palette_index, rgb);
        gl::Uniform3f(sky_u_.sky_color[i], rgb[0], rgb[1], rgb[2]);
        gl::Uniform1f(sky_u_.sky_angle[i], kSkyBands[i].elevation_deg);
    }
    for (size_t i = 0; i < kGroundBands.size(); ++i) {
        float rgb[3];
        sky_band_rgb(scene.palette, kGroundBands[i].palette_index, rgb);
        gl::Uniform3f(sky_u_.ground_color[i], rgb[0], rgb[1], rgb[2]);
        gl::Uniform1f(sky_u_.ground_angle[i], kGroundBands[i].elevation_deg);
    }
    gl::BindVertexArray(sky_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void WorldRenderer::draw(const LevelScene& scene, const WorldFrame& f) {
    if (!program_) return;
    const WorldView& v = f.view;
    glEnable(GL_DEPTH_TEST);
    // The fallback colour, for a level whose palette failed to load or a sky that did not
    // compile: the backdrop covers every pixel otherwise.
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_sky(scene, v);

    gl::UseProgram(program_);
    gl::UniformMatrix4fv(mvp_loc_, 1, GL_FALSE, &v.mvp.m[0][0]);
    gl::Uniform3f(cam_pos_loc_, v.eye.x, v.eye.y, v.eye.z);
    // A straight ramp: full brightness at the camera, raw colour at the radius - the same value
    // as zero point and span is what makes it straight. See shading.h.
    const float radius = shade_radius_world(f.shade_view_cells) * kRenderScale;
    gl::Uniform1f(shade_raw_loc_, radius);
    gl::Uniform1f(shade_span_loc_, radius);
    gl::Uniform1f(atlas_layers_loc_, static_cast<float>(atlas_layers_));
    gl::Uniform1f(shade_strength_loc_, f.shade_strength);
    gl::Uniform1f(shade_dark_ceil_loc_, f.shade_dark_ceil);
    gl::Uniform1f(cull_side_loc_, decor_cull_side());
    if (atlas_) {
        glBindTexture(GL_TEXTURE_2D_ARRAY_, atlas_);
        gl::Uniform1i(atlas_loc_, 0);
    }

    const float r3[3] = {v.right.x, v.right.y, v.right.z};
    const float u3[3] = {v.up.x, v.up.y, v.up.z};

    // THE HORIZON OBJECTS, over the sky and under everything else: no depth test and no depth
    // write, as the original paints them before the world. They follow the
    // camera, so they never come nearer - see LevelScene::horizon_objects.
    if (!scene.horizon_objects.empty()) {
        const float c3[3] = {v.eye.x, v.eye.y, v.eye.z};
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        stream(horizon_, scene.horizon_quads(c3, r3, u3));
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }

    // The terrain, then the decor on the decor's depth bias.
    gl::BindVertexArray(world_.vao);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(terrain_vertices_));
    {
        const DecorBias bias;
        if (vbuf_size_ > terrain_vertices_)
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(terrain_vertices_),
                         static_cast<GLsizei>(vbuf_size_ - terrain_vertices_));

        // Animated decor. Which step shows comes from the WALL CLOCK (LevelAnim::step_at), never
        // from a frame count: the original counted its own 14 ms timer ticks, so the animation
        // runs at one speed whatever the frame rate.
        if (!anim_ranges_.empty()) {
            gl::BindVertexArray(anim_.vao);
            for (size_t sl = 0; sl < anim_ranges_.size(); ++sl) {
                if (anim_ranges_[sl].empty()) continue;
                const int k = scene.anim_step_at(sl, f.wall_ms);
                const std::pair<int, int>& r =
                    anim_ranges_[sl][static_cast<size_t>(k) % anim_ranges_[sl].size()];
                if (r.second > 0) glDrawArrays(GL_TRIANGLES, r.first, r.second);
            }
        }

        // Destructible decor. Which range each instance contributes comes from the shared
        // destruction state, so a prop knocked over by anyone is down for everyone.
        if (!scene.wrecks.empty()) {
            gl::BindVertexArray(wreck_.vao);
            for (const std::pair<int, int>& r : scene.wreck_draw_ranges(f.world, f.sim_tick))
                glDrawArrays(GL_TRIANGLES, r.first, r.second);
        }

        // The start lights - the game's countdown: there is no number on screen in the
        // original, the gantry goes red, red+amber, amber, green.
        const std::pair<int, int>& lr = scene.lights_ranges[static_cast<int>(f.start_light)];
        if (lr.second > 0) {
            gl::BindVertexArray(lights_.vao);
            glDrawArrays(GL_TRIANGLES, lr.first, lr.second);
        }
    }

    // A debugging overlay, depth-tested so a gate behind a hill is behind the hill. Its own
    // vertical lift keeps it off the road (scene/gate_overlay.h).
    if (f.lines && !f.lines->empty()) {
        glLineWidth(2.0f);   // a hint: a core profile may clamp it back to 1, which is fine
        stream(lines_, *f.lines, GL_LINES);
        glLineWidth(1.0f);
    }

    // ⚠️ THE VEHICLES ARE CULLED AND THE WORLD IS NOT. The original draws only the inside of a
    // canopy or windscreen polygon, which is what makes its glass look like glass without a
    // drop of alpha; the decor is drawn both ways round on purpose. GL_FRONT because
    // place_vehicle mirrors world X and reverses every winding with it.
    if (f.vehicles && !f.vehicles->empty()) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        stream(vehicles_, *f.vehicles);
        glDisable(GL_CULL_FACE);
    }
    if (f.effects) stream(effects_, *f.effects);

    // The billboards last - rebuilt every frame since they face the camera.
    if (!scene.sprites.empty()) {
        scratch_ = scene.sprite_quads(r3, u3, f.wall_ms, f.world, f.sim_tick);
        stream(sprites_, scratch_);
    }

    // Then the cabin, over everything - see WorldFrame::cabin. Culled like a vehicle, since it
    // is one: the original's inside-only glass holds here too.
    if (f.cabin && !f.cabin->empty()) {
        glClear(GL_DEPTH_BUFFER_BIT);
        gl::UniformMatrix4fv(mvp_loc_, 1, GL_FALSE, &f.cabin_mvp.m[0][0]);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        stream(cabin_, *f.cabin);
        if (f.cabin_parts && !f.cabin_parts->empty()) {
            glClear(GL_DEPTH_BUFFER_BIT);
            stream(cabin_parts_, *f.cabin_parts);
        }
        glDisable(GL_CULL_FACE);
    }
}

} // namespace bigred
