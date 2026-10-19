// RmlUi's renderer: it hands us indexed triangles, we draw them with our own GL 3.3.
//
// RmlUi embeds NO browser engine and NO renderer - it turns a
// document into geometry and stops there, which is exactly why it was chosen over CEF.
//
// ⚠️ VERTEX COLOURS ARE PREMULTIPLIED (`ColourbPremultiplied`), so the blend is
// `GL_ONE, GL_ONE_MINUS_SRC_ALPHA` and NOT `GL_SRC_ALPHA, ...`. Getting that wrong gives a dark
// halo around everything translucent. Textures we upload are premultiplied on the way in, for
// the same reason.
//
// ⚠️ TEXTURES COME OUT OF THE GAME'S OWN ARCHIVES, not off the filesystem. A document asks for
// one by writing the archive and the member it wants:
//
//     <img src="CONFSCN.SCN/FIRST1.RAW"/>
//
// which is unpacked with the ordinary archive reader and decoded with the ordinary `.RAW`
// decoder - the same two that the level loader and `--canvas` use. A plain name with no `/` is
// looked up in `ALWAYS.WAP`, which is where the shared artwork lives.
#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "formats/game_files.h"
#include "render/gl_loader.h"

namespace bigred {

class RmlRenderInterface : public Rml::RenderInterface {
public:
    ~RmlRenderInterface() override;

    // Compiles the shader. False means no menu can be drawn, and the caller must say so rather
    // than showing an empty window.
    bool init(std::string game_dir);
    void release();
    bool ready() const { return program_ != 0; }

    // The size RmlUi laid out against, in pixels. It is the offscreen canvas' size, not the
    // window's (app/presentation.h), and it must be set before the first frame and whenever it
    // changes.
    void set_viewport(int width, int height);

    // Sets the GL state a document expects, draws nothing. Call around Context::Render.
    void begin_frame();
    void end_frame();

    // A texture that is NOT a file: the font engine's glyph atlas, built in memory. It is
    // registered here under a name, and a document - or the font engine itself, through
    // RenderManager::LoadTexture - then asks for that name like any other source. The bytes are
    // RGBA8, straight (not premultiplied): the upload does that.
    void register_image(std::string name, std::vector<uint8_t> rgba, int width, int height);

    // --- the eight RmlUi must implement -----------------------------------------------------
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

private:
    struct Geometry {
        GLuint vao = 0, vbo = 0, ibo = 0;
        int indices = 0;
    };

    // Archives are unpacked once (formats/game_files.h) and held here, a `.LEV` included - the
    // shared cache keeps only the last two levels, and a picture must not vanish under a menu.
    const Archive* archive(const std::string& name);

    struct Image {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
    };

    std::string game_dir_;
    std::unordered_map<std::string, ArchivePtr> archives_;
    std::unordered_map<std::string, Image> images_;
    std::vector<Geometry*> live_;   // everything compiled, so release() can free it all

    GLuint program_ = 0;
    GLint screen_loc_ = -1, translate_loc_ = -1, tex_loc_ = -1, has_tex_loc_ = -1;
    int width_ = 1, height_ = 1;
    bool scissor_ = false;
};

} // namespace bigred
