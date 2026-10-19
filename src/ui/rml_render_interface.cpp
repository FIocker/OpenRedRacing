#include "rml_render_interface.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "formats/texture.h"   // decode_raw_rgba, kRawWidth/kRawHeight
#include "render/shader.h"

namespace bigred {
namespace {

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColour;
layout(location = 2) in vec2 aUV;
uniform vec2 uScreen;
uniform vec2 uTranslate;
out vec4 vColour;
out vec2 vUV;
void main() {
    // RmlUi lays out in PIXELS from the TOP-LEFT corner; GL is -1..1 from the bottom-left.
    vec2 p = aPos + uTranslate;
    gl_Position = vec4(p.x / uScreen.x * 2.0 - 1.0, 1.0 - p.y / uScreen.y * 2.0, 0.0, 1.0);
    vColour = aColour;
    vUV = aUV;
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec4 vColour;
in vec2 vUV;
uniform sampler2D uTexture;
uniform int uHasTexture;
out vec4 FragColor;
void main() {
    vec4 c = vColour;
    if (uHasTexture == 1) c *= texture(uTexture, vUV);
    FragColor = c;
}
)";

// `ALWAYS.WAP` is where the artwork every screen shares lives, so a bare name resolves there.
constexpr const char* kSharedArchive = "ALWAYS.WAP";

} // namespace

RmlRenderInterface::~RmlRenderInterface() { release(); }

bool RmlRenderInterface::init(std::string game_dir) {
    game_dir_ = std::move(game_dir);
    if (program_) return true;
    program_ = Shader::compile(kVertexShader, kFragmentShader);
    if (!program_) {
        std::fprintf(stderr, "RmlUi: the interface shader failed to compile.\n");
        return false;
    }
    screen_loc_ = gl::GetUniformLocation(program_, "uScreen");
    translate_loc_ = gl::GetUniformLocation(program_, "uTranslate");
    tex_loc_ = gl::GetUniformLocation(program_, "uTexture");
    has_tex_loc_ = gl::GetUniformLocation(program_, "uHasTexture");
    return true;
}

void RmlRenderInterface::release() {
    // ⚠️ RmlUi frees its geometry through ReleaseGeometry, but only for documents it still
    // holds. Whatever is left when a screen goes is freed here, while the context is current.
    for (Geometry* g : live_) {
        if (g->vao) gl::DeleteVertexArrays(1, &g->vao);
        if (g->vbo) gl::DeleteBuffers(1, &g->vbo);
        if (g->ibo) gl::DeleteBuffers(1, &g->ibo);
        delete g;
    }
    live_.clear();
    if (program_) { gl::DeleteProgram(program_); program_ = 0; }
    archives_.clear();
}

void RmlRenderInterface::set_viewport(int width, int height) {
    width_ = width > 0 ? width : 1;
    height_ = height > 0 ? height : 1;
}

void RmlRenderInterface::begin_frame() {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    // ⚠️ PREMULTIPLIED. See the header.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
}

void RmlRenderInterface::end_frame() {
    glDisable(GL_SCISSOR_TEST);
    scissor_ = false;
    glDisable(GL_BLEND);
    gl::UseProgram(0);
    gl::BindVertexArray(0);
}

// --- geometry ---------------------------------------------------------------------------------

Rml::CompiledGeometryHandle RmlRenderInterface::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    Geometry* g = new Geometry();
    g->indices = static_cast<int>(indices.size());

    gl::GenVertexArrays(1, &g->vao);
    gl::BindVertexArray(g->vao);

    gl::GenBuffers(1, &g->vbo);
    gl::BindBuffer(GL_ARRAY_BUFFER_, g->vbo);
    gl::BufferData(GL_ARRAY_BUFFER_,
                   static_cast<GLsizeiptr>(vertices.size() * sizeof(Rml::Vertex)),
                   vertices.data(), GL_STATIC_DRAW_);

    gl::GenBuffers(1, &g->ibo);
    gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER_, g->ibo);
    gl::BufferData(GL_ELEMENT_ARRAY_BUFFER_,
                   static_cast<GLsizeiptr>(indices.size() * sizeof(int)), indices.data(),
                   GL_STATIC_DRAW_);

    // Rml::Vertex is {Vector2f position; ColourbPremultiplied colour; Vector2f tex_coord;} -
    // laid out exactly like this, which is why it can be uploaded without a copy.
    const GLsizei stride = static_cast<GLsizei>(sizeof(Rml::Vertex));
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                            reinterpret_cast<void*>(offsetof(Rml::Vertex, position)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                            reinterpret_cast<void*>(offsetof(Rml::Vertex, colour)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                            reinterpret_cast<void*>(offsetof(Rml::Vertex, tex_coord)));
    gl::BindVertexArray(0);

    live_.push_back(g);
    return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

void RmlRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                        Rml::Vector2f translation, Rml::TextureHandle texture) {
    if (!program_) return;
    Geometry* g = reinterpret_cast<Geometry*>(geometry);
    if (!g || g->indices == 0) return;

    gl::UseProgram(program_);
    if (screen_loc_ >= 0)
        gl::Uniform2f(screen_loc_, static_cast<float>(width_), static_cast<float>(height_));
    if (translate_loc_ >= 0) gl::Uniform2f(translate_loc_, translation.x, translation.y);
    if (has_tex_loc_ >= 0) gl::Uniform1i(has_tex_loc_, texture ? 1 : 0);
    if (texture) {
        gl::ActiveTexture(GL_TEXTURE0_);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        if (tex_loc_ >= 0) gl::Uniform1i(tex_loc_, 0);
    }
    gl::BindVertexArray(g->vao);
    glDrawElements(GL_TRIANGLES, g->indices, GL_UNSIGNED_INT, nullptr);
    gl::BindVertexArray(0);
}

void RmlRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    Geometry* g = reinterpret_cast<Geometry*>(geometry);
    if (!g) return;
    live_.erase(std::remove(live_.begin(), live_.end(), g), live_.end());
    if (g->vao) gl::DeleteVertexArrays(1, &g->vao);
    if (g->vbo) gl::DeleteBuffers(1, &g->vbo);
    if (g->ibo) gl::DeleteBuffers(1, &g->ibo);
    delete g;
}

// --- textures ---------------------------------------------------------------------------------

const Archive* RmlRenderInterface::archive(const std::string& name) {
    auto it = archives_.find(name);
    if (it != archives_.end()) return it->second.get();
    try {
        return (archives_[name] = game_archive(game_dir_, name)).get();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "RmlUi: cannot open %s (%s)\n", name.c_str(), ex.what());
        return nullptr;
    }
}

void RmlRenderInterface::register_image(std::string name, std::vector<uint8_t> rgba, int width,
                                        int height) {
    Image img;
    img.rgba = std::move(rgba);
    img.w = width;
    img.h = height;
    images_[std::move(name)] = std::move(img);
}

Rml::TextureHandle RmlRenderInterface::LoadTexture(Rml::Vector2i& dimensions,
                                                   const Rml::String& source) {
    // A registered image first, by its exact name: it is ours, not the game's, and it must not
    // be run through the archive lookup below.
    if (auto img = images_.find(std::string(source.c_str())); img != images_.end()) {
        dimensions = Rml::Vector2i(img->second.w, img->second.h);
        return GenerateTexture(
            Rml::Span<const Rml::byte>(img->second.rgba.data(), img->second.rgba.size()),
            dimensions);
    }

    // "ARCHIVE.EXT/MEMBER" - or just "MEMBER", which means ALWAYS.WAP. See the header.
    std::string src(source.c_str());
    std::string archive_name = kSharedArchive;
    std::string member = src;
    const size_t slash = src.find_last_of("/\\");
    if (slash != std::string::npos) {
        archive_name = src.substr(0, slash);
        member = src.substr(slash + 1);
    }
    for (char& c : archive_name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (char& c : member) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    const auto* files = archive(archive_name);
    if (!files) return 0;
    auto it = files->find(member);
    if (it == files->end()) {
        std::fprintf(stderr, "RmlUi: %s holds no %s\n", archive_name.c_str(), member.c_str());
        return 0;
    }

    // Only `.RAW` for now - the full-screen backgrounds. A `.SPR` is a SHEET of glyphs with
    // per-glyph sizes, so it belongs to the font engine and to a sprite element, not here.
    const std::vector<uint8_t> rgba = decode_raw_rgba(it->second.data(), it->second.size());
    if (rgba.empty()) {
        std::fprintf(stderr, "RmlUi: %s is not a .RAW this decoder knows\n", member.c_str());
        return 0;
    }
    dimensions = Rml::Vector2i(kRawWidth, kRawHeight);
    return GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), dimensions);
}

Rml::TextureHandle RmlRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                       Rml::Vector2i dimensions) {
    if (dimensions.x <= 0 || dimensions.y <= 0) return 0;
    const size_t needed = static_cast<size_t>(dimensions.x) * dimensions.y * 4;
    if (source.size() < needed) return 0;

    // ⚠️ PREMULTIPLIED ON THE WAY IN, to match the vertex colours and the blend mode. RmlUi's
    // own font atlas would already be premultiplied; ours are decoded from 1996 artwork, where
    // alpha is "this pixel is the transparent index" and nothing has been multiplied.
    std::vector<uint8_t> pma(needed);
    for (size_t i = 0; i < needed; i += 4) {
        const uint32_t a = source[i + 3];
        pma[i] = static_cast<uint8_t>(source[i] * a / 255);
        pma[i + 1] = static_cast<uint8_t>(source[i + 1] * a / 255);
        pma[i + 2] = static_cast<uint8_t>(source[i + 2] * a / 255);
        pma[i + 3] = static_cast<uint8_t>(a);
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dimensions.x, dimensions.y, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pma.data());
    // NEAREST: every pixel in here is 1996 artwork. The HUD learned this the hard way - see
    // render/hud.h, "les nouvelles ecritures sont illisibles".
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    glBindTexture(GL_TEXTURE_2D, 0);
    return static_cast<Rml::TextureHandle>(texture);
}

void RmlRenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
    GLuint t = static_cast<GLuint>(texture);
    if (t) glDeleteTextures(1, &t);
}

// --- clipping -----------------------------------------------------------------------------

void RmlRenderInterface::EnableScissorRegion(bool enable) {
    scissor_ = enable;
    if (enable) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
}

void RmlRenderInterface::SetScissorRegion(Rml::Rectanglei region) {
    // ⚠️ THE Y FLIP AGAIN: RmlUi counts from the top, glScissor from the bottom.
    glScissor(region.Left(), height_ - region.Bottom(), region.Width(), region.Height());
}

} // namespace bigred
