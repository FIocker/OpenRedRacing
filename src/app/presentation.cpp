#include "presentation.h"

#include "render/shader.h"

#include <algorithm>
#include <cstdio>

namespace bigred {
namespace {

constexpr const char* kVertexShader = R"(#version 330 core
out vec2 vUV;
// The part of the source texture that is used, from its bottom-left corner: (1,1) for a whole
// buffer, less for a model drawn in the corner of the model buffer (draw_model).
uniform vec2 uUVScale;
void main() {
    // One full-screen triangle; the viewport already restricts it to the target rectangle.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p * uUVScale;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// A plain copy. There used to be a menu filter here that quantised the whole canvas to 320x200
// at the lowest rung; it turned every steady text into mush and was dropped on 2026-09-27 at
// the user's request - the ladder is for the game and the 3D models, not for the text (see
// presentation.h).
constexpr const char* kFragmentShader = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uCanvas;
out vec4 FragColor;
void main() { FragColor = texture(uCanvas, vUV); }
)";

} // namespace

// --- one offscreen buffer ---------------------------------------------------------------------

void Presentation::Target::release() {
    if (colour) { glDeleteTextures(1, &colour); colour = 0; }
    if (depth) { gl::DeleteRenderbuffers(1, &depth); depth = 0; }
    if (fbo) { gl::DeleteFramebuffers(1, &fbo); fbo = 0; }
    w = h = 0;
}

bool Presentation::Target::ensure(int want_w, int want_h, bool want_depth, bool linear) {
    if (want_w <= 0 || want_h <= 0) return false;
    if (fbo && want_w == w && want_h == h && (depth != 0) == want_depth) return true;
    release();

    glGenTextures(1, &colour);
    glBindTexture(GL_TEXTURE_2D, colour);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, want_w, want_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // LINEAR for the menus: the filter samples one point per cell, and a linear tap there
    // averages the few texels around it - with NEAREST a cell would take one raw pixel and
    // sparkle as the window resizes. NEAREST for the world, where the big pixels ARE the point
    // and smoothing them would just look like a blurry modern render.
    const GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    glBindTexture(GL_TEXTURE_2D, 0);

    gl::GenFramebuffers(1, &fbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER_, fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, colour, 0);
    if (want_depth) {
        gl::GenRenderbuffers(1, &depth);
        gl::BindRenderbuffer(GL_RENDERBUFFER_, depth);
        gl::RenderbufferStorage(GL_RENDERBUFFER_, GL_DEPTH_COMPONENT24_, want_w, want_h);
        gl::BindRenderbuffer(GL_RENDERBUFFER_, 0);
        gl::FramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_DEPTH_ATTACHMENT_, GL_RENDERBUFFER_,
                                    depth);
    }
    const GLenum status = gl::CheckFramebufferStatus(GL_FRAMEBUFFER_);
    gl::BindFramebuffer(GL_FRAMEBUFFER_, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE_) {
        std::fprintf(stderr, "Presentation: incomplete framebuffer (0x%x)\n", status);
        release();
        return false;
    }
    w = want_w;
    h = want_h;
    return true;
}

// --- the ladder ---------------------------------------------------------------------------

void Presentation::set_mode(int index) {
    if (index < 0) index = 0;
    if (index >= kDisplayModeCount) index = kDisplayModeCount - 1;
    mode_ = index;
}

void Presentation::step_mode(int delta) { set_mode(mode_ + delta); }

// --- lifetime -----------------------------------------------------------------------------

Presentation::~Presentation() { release(); }

void Presentation::release() {
    canvas_.release();
    world_.release();
    model_.release();
    if (vao_) { gl::DeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (program_) { gl::DeleteProgram(program_); program_ = 0; }
}

bool Presentation::init() {
    if (program_) return true;
    program_ = Shader::compile(kVertexShader, kFragmentShader);
    if (!program_) {
        std::fprintf(stderr, "Presentation: blit shader failed to compile.\n");
        return false;
    }
    tex_loc_ = gl::GetUniformLocation(program_, "uCanvas");
    uv_scale_loc_ = gl::GetUniformLocation(program_, "uUVScale");
    gl::GenVertexArrays(1, &vao_);
    return true;
}

// --- the menus ------------------------------------------------------------------------------

void Presentation::begin_canvas(int window_w, int window_h) {
    if (!ready()) return;
    const Box b = box_for(window_w, window_h);
    // ⚠️ WITH A DEPTH BUFFER, since 2026-09-23. The menus themselves are flat and draw with the
    // depth test off, but a menu can now show a 3D MODEL - the vehicle turning on the car
    // screen (ui/vehicle_display.h) - and a car is not convex: back-face culling alone leaves
    // its far wheels drawn over its near bodywork. The globe got away without one because a
    // sphere IS convex (ui/globe.h).
    if (!canvas_.ensure(b.w, b.h, true, true)) return;
    gl::BindFramebuffer(GL_FRAMEBUFFER_, canvas_.fbo);
    glViewport(0, 0, canvas_.w, canvas_.h);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // Depth is cleared here, once, and never again: nothing writes to it unless it also enables
    // the depth test, so whatever draws a model can simply switch the test on and trust it.
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

bool Presentation::read_canvas(std::vector<uint8_t>& rgba, int& w, int& h) const {
    if (!canvas_.fbo || canvas_.w <= 0 || canvas_.h <= 0) return false;
    w = canvas_.w;
    h = canvas_.h;
    std::vector<uint8_t> raw(static_cast<size_t>(w) * h * 4);
    gl::BindFramebuffer(GL_FRAMEBUFFER_, canvas_.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    gl::BindFramebuffer(GL_FRAMEBUFFER_, 0);
    // GL's rows run bottom-up.
    rgba.resize(raw.size());
    const size_t row = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; ++y)
        std::copy_n(raw.data() + static_cast<size_t>(h - 1 - y) * row, row, rgba.data() + static_cast<size_t>(y) * row);
    return true;
}

Presentation::Box Presentation::box_for(int window_w, int window_h) {
    Box b;
    if (window_w <= 0 || window_h <= 0) return b;
    // 4:3 - the shape of the screen the original was played on. The menus are not bound to its
    // RESOLUTION any more, only to its framing.
    b.w = window_w;
    b.h = window_w * 3 / 4;
    if (b.h > window_h) {
        b.h = window_h;
        b.w = window_h * 4 / 3;
    }
    b.x = (window_w - b.w) / 2;
    b.y = (window_h - b.h) / 2;
    return b;
}

bool Presentation::to_canvas(int window_w, int window_h, int mx, int my, float& cx, float& cy) {
    const Box b = box_for(window_w, window_h);
    if (b.w <= 0 || b.h <= 0) return false;
    if (mx < b.x || my < b.y || mx >= b.x + b.w || my >= b.y + b.h) return false;
    cx = static_cast<float>(mx - b.x);
    cy = static_cast<float>(my - b.y);
    return true;
}

void Presentation::present(int window_w, int window_h) {
    if (!ready() || !canvas_.fbo) return;
    gl::BindFramebuffer(GL_FRAMEBUFFER_, 0);
    glViewport(0, 0, window_w, window_h);
    glDisable(GL_DEPTH_TEST);
    // The letterbox. Black, not the canvas' edge colour: a menu that does not fill the box is a
    // layout bug, and smearing the border would hide it.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const Box b = box_for(window_w, window_h);
    glViewport(b.x, b.y, b.w, b.h);
    gl::UseProgram(program_);
    gl::ActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D, canvas_.colour);
    if (tex_loc_ >= 0) gl::Uniform1i(tex_loc_, 0);
    if (uv_scale_loc_ >= 0) gl::Uniform2f(uv_scale_loc_, 1.0f, 1.0f);
    gl::BindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
    gl::UseProgram(0);
    glViewport(0, 0, window_w, window_h);
}

// --- the world ------------------------------------------------------------------------------

bool Presentation::begin_world(int window_w, int window_h) {
    if (!ready() || lines() <= 0 || window_w <= 0 || window_h <= 0) return false;
    // ⚠️ THE HEIGHT IS THE SETTING AND THE WIDTH FOLLOWS THE WINDOW. Forcing 320x200 onto a
    // 16:9 window would either squash the picture or letterbox a game that has no reason to be
    // letterboxed; what a player recognises is the SIZE of a pixel, and that is the line count.
    const int h = std::min(lines(), window_h);
    int w = static_cast<int>(static_cast<long long>(h) * window_w / window_h);
    if (w < 1) w = 1;
    const bool resized = (w != world_.w || h != world_.h);
    if (!world_.ensure(w, h, true, false)) return false;
    // One line per resolution change: it is the only way to tell "the world really rendered at
    // 200 lines" from "the key did nothing" without a screenshot. Flushed, because Windows
    // buffers stdout fully as soon as it is a pipe - a status line that only shows up once the
    // game has exited is a status line nobody reads.
    if (resized) {
        std::printf("World target: %dx%d (%s)\n", w, h, mode_name());
        std::fflush(stdout);
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER_, world_.fbo);
    glViewport(0, 0, world_.w, world_.h);
    return true;
}

void Presentation::present_world(int window_w, int window_h) {
    if (!ready() || !world_.fbo) return;
    gl::BindFramebuffer(GL_FRAMEBUFFER_, 0);
    glViewport(0, 0, window_w, window_h);
    glDisable(GL_DEPTH_TEST);
    gl::UseProgram(program_);
    gl::ActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D, world_.colour);
    if (tex_loc_ >= 0) gl::Uniform1i(tex_loc_, 0);
    // The buffer IS the low resolution, and NEAREST magnification is what turns it into big
    // square pixels.
    if (uv_scale_loc_ >= 0) gl::Uniform2f(uv_scale_loc_, 1.0f, 1.0f);
    gl::BindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
    gl::UseProgram(0);
}

// --- a 3D model inside a menu ----------------------------------------------------------------

void Presentation::draw_model(float x, float y, float w, float h, const ModelDraw& draw) {
    const int cw = canvas_.w, ch = canvas_.h;
    if (cw <= 0 || ch <= 0 || w < 1.0f || h < 1.0f) return;
    // The cell is square: canvas_h / lines real pixels, the size a pixel of the race has at the
    // same rung. At one pixel or less there is nothing to do - the model draws straight into
    // the canvas.
    const float cell = lines() > 0 ? static_cast<float>(ch) / static_cast<float>(lines()) : 1.0f;
    if (!ready() || cell <= 1.0f) {
        draw(cw, ch, x, y, w, h);
        return;
    }
    const int pw = std::max(1, static_cast<int>(w / cell + 0.5f));
    const int ph = std::max(1, static_cast<int>(h / cell + 0.5f));
    // ONE buffer for every model of the frame, the size of the whole canvas at this rung: a
    // model uses its bottom-left corner. Sized per model it would be reallocated for each of
    // the lobby's cars, every frame.
    const int bw = std::max(pw, static_cast<int>(static_cast<float>(cw) / cell + 0.5f));
    const int bh = std::max(ph, lines());
    if (!model_.ensure(bw, bh, true, false)) {
        draw(cw, ch, x, y, w, h);
        return;
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER_, model_.fbo);
    glViewport(0, 0, pw, ph);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, pw, ph);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    draw(pw, ph, 0.0f, 0.0f, static_cast<float>(pw), static_cast<float>(ph));

    // Back onto the canvas, over whatever is already there: the model wrote an opaque alpha
    // where it drew and the clear left 0 elsewhere, so the artwork shows around it.
    gl::BindFramebuffer(GL_FRAMEBUFFER_, canvas_.fbo);
    glViewport(static_cast<GLint>(x), static_cast<GLint>(static_cast<float>(ch) - (y + h)),
               static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl::UseProgram(program_);
    gl::ActiveTexture(GL_TEXTURE0_);
    glBindTexture(GL_TEXTURE_2D, model_.colour);
    if (tex_loc_ >= 0) gl::Uniform1i(tex_loc_, 0);
    if (uv_scale_loc_ >= 0)
        gl::Uniform2f(uv_scale_loc_, static_cast<float>(pw) / static_cast<float>(model_.w),
                      static_cast<float>(ph) / static_cast<float>(model_.h));
    gl::BindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
    gl::UseProgram(0);
    glDisable(GL_BLEND);
    glViewport(0, 0, cw, ch);
}

} // namespace bigred
