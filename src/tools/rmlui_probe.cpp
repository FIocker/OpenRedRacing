// `rmlui_probe` - does RmlUi still work with this toolchain?
//
// Not part of the game and not built by default: `cmake --build build --target rmlui_probe`.
// It answers, in one command, the question that has to be re-answered every time RmlUi or the
// compiler moves:
//
//   * do RmlUi's headers compile under MinGW/GCC in C++20;
//   * can we derive from the three interfaces the integration needs - renderer, system, font;
//   * does it link STATICALLY, with no FreeType anywhere (RMLUI_FONT_ENGINE=none);
//   * does it actually run: parse RML/RCSS, lay out flexbox, with no font engine doing any
//     real work yet.
//
// It is kept as a target rather than written as a throwaway for the same reason physics_bench
// is: the measurement behind a dependency decision has to be REPEATABLE. Verified 2026-09-22
// against RmlUi 6.3 and GCC 16.1.
//
// ⚠️ The stubs below are STUBS. The real interfaces live in src/ui/; this file must stay a
// compile-and-run check and must not grow into the integration.

#include <RmlUi/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <cstdio>

// --- the renderer: the eight pure virtuals, nothing else -----------------------------------
class ProbeRenderer : public Rml::RenderInterface {
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        // The vertex format we would upload: position in pixels, PREMULTIPLIED rgba, uv.
        (void)vertices;
        (void)indices;
        return 1;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        (void)geometry; (void)translation; (void)texture;
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override { (void)geometry; }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dims, const Rml::String& source) override {
        // This is where a `.RAW` / `.SPR` out of the game archives would be decoded.
        (void)source;
        dims = Rml::Vector2i(1, 1);
        return 1;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i dims) override {
        (void)source; (void)dims;
        return 1;
    }
    void ReleaseTexture(Rml::TextureHandle texture) override { (void)texture; }

    void EnableScissorRegion(bool enable) override { (void)enable; }
    void SetScissorRegion(Rml::Rectanglei region) override { (void)region; }
};

// --- the system interface: clock + log ------------------------------------------------------
class ProbeSystem : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { return 0.0; }
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        (void)type; (void)message;
        return true;
    }
};

// --- the font engine: what replaces FreeType, over the game's own .SPR sheets ---------------
class ProbeFont : public Rml::FontEngineInterface {
public:
    bool LoadFontFace(const Rml::String& file_name, int face_index, bool fallback_face,
                      Rml::Style::FontWeight weight) override {
        (void)file_name; (void)face_index; (void)fallback_face; (void)weight;
        return true;
    }
    Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style,
                                          Rml::Style::FontWeight weight, int size) override {
        (void)family; (void)style; (void)weight; (void)size;
        return 1;
    }
    const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override {
        (void)handle;
        static Rml::FontMetrics metrics{};
        return metrics;
    }
    int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string,
                       const Rml::TextShapingContext& ctx, Rml::Character prior) override {
        (void)handle; (void)string; (void)ctx; (void)prior;
        return 0;
    }
    int GenerateString(Rml::RenderManager& rm, Rml::FontFaceHandle face,
                       Rml::FontEffectsHandle effects, Rml::StringView string,
                       Rml::Vector2f position, Rml::ColourbPremultiplied colour, float opacity,
                       const Rml::TextShapingContext& ctx, Rml::TexturedMeshList& mesh_list) override {
        (void)rm; (void)face; (void)effects; (void)string; (void)position; (void)colour;
        (void)opacity; (void)ctx; (void)mesh_list;
        return 0;
    }
};

int main() {
    ProbeRenderer renderer;
    ProbeSystem system;
    ProbeFont font;

    Rml::SetRenderInterface(&renderer);
    Rml::SetSystemInterface(&system);
    Rml::SetFontEngineInterface(&font);

    if (!Rml::Initialise()) {
        std::printf("PROBE: Rml::Initialise() FAILED\n");
        return 1;
    }
    Rml::Context* ctx = Rml::CreateContext("probe", Rml::Vector2i(1280, 720));
    if (!ctx) {
        std::printf("PROBE: CreateContext FAILED\n");
        Rml::Shutdown();
        return 1;
    }
    // A document from a string: proves the RML/RCSS parser runs with no font engine and no
    // FreeType anywhere in the binary.
    Rml::ElementDocument* doc = ctx->LoadDocumentFromMemory(
        "<rml><head><style>"
        "body { width: 320px; height: 200px; background: #204080; display: flex; }"
        "div  { width: 40px; height: 40px; background: #ff8000; margin: 4px; }"
        "</style></head><body><div/><div/><div/></body></rml>");
    if (!doc) {
        std::printf("PROBE: LoadDocumentFromMemory FAILED\n");
        Rml::Shutdown();
        return 1;
    }
    doc->Show();
    ctx->Update();
    ctx->Render();

    std::printf("PROBE OK: initialised, context created, document parsed and laid out.\n");
    std::printf("PROBE OK: body is %.0f x %.0f px, %d child element(s).\n",
                doc->GetBox().GetSize().x, doc->GetBox().GetSize().y, doc->GetNumChildren());
    Rml::Shutdown();
    return 0;
}
