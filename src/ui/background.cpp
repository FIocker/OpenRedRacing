#include "background.h"

#include "formats/texture.h"
#include "render/shader.h"

#include <cstdio>

namespace bigred {
namespace {

constexpr const char* kVertexShader = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    // ⚠️ The ONE V flip on this path, and it belongs here. A `.RAW` stores its TOP row first
    // (confirmed by eye on four different backgrounds - fireworks rise, the trumpeter is the
    // right way up, "TEAMtalk" is not mirrored), so row 0 uploads to v=0, while the canvas is
    // GL-natural with v=0 at the BOTTOM. Flipping on the way in leaves the canvas consistent
    // with what Hud draws, and Presentation then needs no flip at all.
    vUV = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uImage;
out vec4 FragColor;
void main() { FragColor = texture(uImage, vUV); }
)";

} // namespace

Background::~Background() { release(); }

void Background::release() {
    if (texture_) { glDeleteTextures(1, &texture_); texture_ = 0; }
    if (vao_) { gl::DeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (program_) { gl::DeleteProgram(program_); program_ = 0; }
}

bool Background::load(const std::vector<uint8_t>& raw) {
    release();
    const std::vector<uint8_t> rgba = decode_raw_rgba(raw.data(), raw.size());
    if (rgba.empty()) {
        std::fprintf(stderr, "Background: not a %zu-byte .RAW (%zu bytes)\n", kRawSize, raw.size());
        return false;
    }

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kRawWidth, kRawHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    // NEAREST: the canvas is the image's own size, so this samples 1:1 and the filter never
    // actually interpolates. It is set anyway, because a future scaled layout must not quietly
    // start blurring the artwork - the whole 4:3 stretch happens later, in Presentation.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    glBindTexture(GL_TEXTURE_2D, 0);

    program_ = Shader::compile(kVertexShader, kFragmentShader);
    if (!program_) {
        release();
        return false;
    }
    tex_loc_ = gl::GetUniformLocation(program_, "uImage");
    gl::GenVertexArrays(1, &vao_);
    return true;
}

void Background::draw() {
    if (!ready()) return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    gl::UseProgram(program_);
    gl::ActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (tex_loc_ >= 0) gl::Uniform1i(tex_loc_, 0);
    gl::BindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
    gl::UseProgram(0);
}

} // namespace bigred
