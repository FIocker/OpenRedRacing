// Vehicle preview tool - reproduces src/main.cpp's original milestone-2 vehicle render.
// Kept as a separate small executable so the main track-scene
// renderer (src/main.cpp) doesn't need vehicle-loading code mixed in.
//
// Usage: vehicle_preview [game_dir] [VHC_NAME] [OBJECT_NAME]  (defaults: JEEP JEEP1)
//
//        vehicle_preview --dump [game_dir] [VHC_NAME]
//            No window: prints, for EVERY shape of a `.VHC`, what each face is made of - its
//            colour or lack of one, whether it carries its own inline UV, and which crop the
//            display list hands it. That is the whole of what tells a wheel RIM from a LOGO
//            SLOT, and both look identical from outside ("a face with no palette colour").
//            Written on 2026-09-23 after the vehicle renderer painted logo slots with whatever
//            crop came to hand, which put the wrong emblem on the boats and made the buggy's
//            nose z-fight with itself.

#include <SDL.h>
#include <SDL_opengl.h>

#include "formats/game_files.h"
#include "formats/threedw.h"
#include "render/gl_loader.h"
#include "render/mat4.h"
#include "render/shader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Vertex {
    float x, y, z;
    float r, g, b;
};

// Fan-triangulates every face (matches the flat n-gon faces this format uses).
std::vector<Vertex> build_vertex_buffer(const bigred::ThreeDW& model, const std::vector<bigred::RGB>& palette,
                                         int& skipped_out) {
    constexpr float SCALE = 1.0f / 300.0f;
    std::vector<Vertex> out;
    skipped_out = 0;

    for (const bigred::Face& face : model.faces) {
        if (face.indices.size() < 3) continue;

        float r, g, b;
        if (face.color.has_value() && *face.color >= 0 &&
            static_cast<size_t>(*face.color) < palette.size()) {
            const bigred::RGB& c = palette[static_cast<size_t>(*face.color)];
            r = c.r / 255.0f; g = c.g / 255.0f; b = c.b / 255.0f;
        } else {
            // Logo-slot / textured face - no colour resolvable from the file alone. Magenta
            // placeholder, same convention as tools/visualize_3dw.py.
            r = 0.78f; g = 0.24f; b = 0.78f;
        }

        auto vert = [&](int32_t idx) -> Vertex {
            const bigred::Vec3i& v = model.vertices[static_cast<size_t>(idx)];
            // .3DW local Y is stored inverted (-Y = up) relative to this renderer's world/view
            // convention (+Y = up) - see threedw.h. Confirmed visually on CHTREE (a lopsided
            // cone that only points up, not down into the ground, once negated).
            return Vertex{v.x * SCALE, -v.y * SCALE, v.z * SCALE, r, g, b};
        };

        for (size_t i = 1; i + 1 < face.indices.size(); ++i) {
            out.push_back(vert(face.indices[0]));
            out.push_back(vert(face.indices[i]));
            out.push_back(vert(face.indices[i + 1]));
        }
    }
    return out;
}

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec3 vColor;
out vec4 FragColor;
void main() {
    FragColor = vec4(vColor, 1.0);
}
)";

} // namespace

// The diagnostic mode. Prints and returns; opens nothing.
static int dump_vhc(const std::string& game_dir, const std::string& vhc_name) {
    bigred::Archive vhc;
    try {
        vhc = *bigred::game_archive(game_dir, vhc_name + ".VHC");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s.VHC: %s\n", vhc_name.c_str(), e.what());
        return 1;
    }
    std::vector<std::string> names;
    for (const auto& kv : vhc)
        if (kv.first.size() > 4 && kv.first.compare(kv.first.size() - 4, 4, ".3DW") == 0)
            names.push_back(kv.first);
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        const bigred::Blob& blob = vhc[name];
        bigred::ThreeDW m;
        try {
            m = bigred::parse_3dw(blob.data(), blob.size());
        } catch (const std::exception& e) {
            std::printf("%-14s PARSE FAILED: %s\n", name.c_str(), e.what());
            continue;
        }
        const bigred::DisplayListReplay replay = bigred::replay_display_list(
            m.branches, m.face_stream_pos, m.crop_stream_pos, m.faces.size(), 0);
        int colourless = 0, own_uv = 0, with_crop = 0, no_crop = 0;
        for (size_t i = 0; i < m.faces.size(); ++i) {
            const bigred::Face& f = m.faces[i];
            if (f.color.has_value()) continue;
            ++colourless;
            const bool inline_uv = !f.uv.empty() && f.uv.size() == f.indices.size();
            if (inline_uv) ++own_uv;
            const int c = (i < replay.crop.size()) ? replay.crop[i] : -1;
            if (!inline_uv) (c >= 0 ? ++with_crop : ++no_crop);
        }
        std::printf("%-14s faces=%-4zu tex='%s' variants=%-3zu drawn_n=%-4zu | colourless=%-3d "
                    "own_uv=%-3d crop=%-3d none=%-3d\n",
                    name.c_str(), m.faces.size(), m.embedded_texture_name.c_str(),
                    m.texture_variants.size(), replay.drawn.size(), colourless,
                    own_uv, with_crop, no_crop);
        for (size_t i = 0; i < m.faces.size(); ++i) {
            const bigred::Face& f = m.faces[i];
            if (f.color.has_value()) continue;
            const bool inline_uv = !f.uv.empty() && f.uv.size() == f.indices.size();
            const int c = (i < replay.crop.size()) ? replay.crop[i] : -1;
            const char* reach = (i >= replay.drawn.size()) ? "OUT-OF-TABLE"
                                                          : (replay.drawn[i] ? "yes" : "no");
            // Is it reached by ANY variant byte? A face the default walk steps over may
            // still belong to a branch the game takes for some state - or to none at all,
            // in which case the shipped game never draws it either.
            int reached_by = 0, first_variant = -1;
            for (int variant = 0; variant < 256; ++variant) {
                const bigred::DisplayListReplay r = bigred::replay_display_list(
                    m.branches, m.face_stream_pos, m.crop_stream_pos, m.faces.size(), variant);
                if (i < r.drawn.size() && r.drawn[i]) {
                    ++reached_by;
                    if (first_variant < 0) first_variant = variant;
                }
            }
            std::printf("      face %-3zu verts=%-2zu tag5_uv=%d texvar=%d crop=%-3d reached=%-12s by %3d/256 (first %3d)",
                        i, f.indices.size(), inline_uv ? 1 : 0,
                        f.textured_variant ? 1 : 0, c, reach, reached_by, first_variant);
            // Where it samples, in raw .TM texels - the number that says WHICH emblem of
            // the sheet's baked grid this slot shows.
            if (inline_uv) {
                float u0 = f.uv[0][0], v0 = f.uv[0][1], u1 = u0, v1 = v0;
                for (const auto& uv : f.uv) {
                    u0 = std::min(u0, uv[0]); u1 = std::max(u1, uv[0]);
                    v0 = std::min(v0, uv[1]); v1 = std::max(v1, uv[1]);
                }
                std::printf("  uv=[%.0f,%.0f .. %.0f,%.0f] %.0fx%.0f", u0, v0, u1, v1,
                            u1 - u0, v1 - v0);
            } else if (c >= 0 && static_cast<size_t>(c) < m.texture_variants.size()) {
                const bigred::UVVariant& cr = m.texture_variants[static_cast<size_t>(c)];
                std::printf("  crop=[%.0f,%.0f .. %.0f,%.0f]", cr.u0, cr.v0, cr.u1, cr.v1);
            }
            std::printf("\n");
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--dump") {
        const std::string dir = (argc > 2) ? argv[2] : bigred::find_game_dir();
        return dump_vhc(dir, (argc > 3) ? argv[3] : "JEEP");
    }
    std::string game_dir = (argc > 1) ? argv[1] : bigred::find_game_dir();
    std::string vhc_name = (argc > 2) ? argv[2] : "JEEP";
    std::string obj_name = (argc > 3) ? argv[3] : "JEEP1";

    bigred::ThreeDW model;
    std::vector<bigred::RGB> palette;
    try {
        const bigred::Archive& vhc = *bigred::game_archive(game_dir, vhc_name + ".VHC");
        const bigred::Archive& wap = *bigred::game_archive(game_dir, "ALWAYS.WAP");

        auto it = vhc.find(obj_name + ".3DW");
        if (it == vhc.end()) throw std::runtime_error(obj_name + ".3DW not found in " + vhc_name + ".VHC");
        model = bigred::parse_3dw(it->second.data(), it->second.size());

        auto col = wap.find("DEFAULT.COL");
        if (col == wap.end()) throw std::runtime_error("DEFAULT.COL not found in ALWAYS.WAP");
        palette = bigred::load_palette_rgb(col->second.data(), col->second.size());

        std::printf("%s.3DW: %zu vertices, %d/%d faces parsed, %zu lines, clean_end=%d",
                    obj_name.c_str(), model.vertices.size(), model.parsed_face_count,
                    model.expected_face_count, model.lines.size(), model.reached_clean_end ? 1 : 0);
        if (!model.reached_clean_end) {
            std::printf(" (stopped at byte %zu, tag=%d)", model.stop_byte_offset, model.stop_tag);
        }
        std::printf("\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to load %s.3DW from '%s': %s\n"
                              "(usage: vehicle_preview [game_dir] [VHC_NAME] [OBJECT_NAME])\n",
                     obj_name.c_str(), game_dir.c_str(), e.what());
        return 1;
    }

    int skipped = 0;
    std::vector<Vertex> vbuf = build_vertex_buffer(model, palette, skipped);
    std::printf("Vertex buffer: %zu vertices (%zu triangles), %d degenerate face(s) skipped\n",
                vbuf.size(), vbuf.size() / 3, skipped);

    // Auto-frame the camera from the (already render-scaled) vertex buffer's own bounding box,
    // rather than a hardcoded guess - keeps the model filling most of the window regardless of
    // its actual size, so defects like the fixed spike are actually visible up close.
    float bmin[3] = {vbuf[0].x, vbuf[0].y, vbuf[0].z};
    float bmax[3] = {vbuf[0].x, vbuf[0].y, vbuf[0].z};
    for (const Vertex& v : vbuf) {
        bmin[0] = std::min(bmin[0], v.x); bmax[0] = std::max(bmax[0], v.x);
        bmin[1] = std::min(bmin[1], v.y); bmax[1] = std::max(bmax[1], v.y);
        bmin[2] = std::min(bmin[2], v.z); bmax[2] = std::max(bmax[2], v.z);
    }
    float center[3] = {(bmin[0] + bmax[0]) * 0.5f, (bmin[1] + bmax[1]) * 0.5f, (bmin[2] + bmax[2]) * 0.5f};
    float radius = 0.0f;
    for (int i = 0; i < 3; ++i) radius = std::max(radius, (bmax[i] - bmin[i]) * 0.5f);
    float orbit_distance = radius * 2.6f + 0.1f; // comfortably fits a 60deg-FOV frame with margin

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* window = SDL_CreateWindow(
        "OpenBigRed - vehicle preview",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    if (!gl::load()) {
        std::fprintf(stderr, "Failed to load required OpenGL 3.3 functions.\n");
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    GLuint program = Shader::compile(kVertexShader, kFragmentShader);
    if (!program) {
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    GLint mvpLoc = gl::GetUniformLocation(program, "uMVP");

    GLuint vao = 0, vbo = 0;
    gl::GenVertexArrays(1, &vao);
    gl::BindVertexArray(vao);
    gl::GenBuffers(1, &vbo);
    gl::BindBuffer(GL_ARRAY_BUFFER_, vbo);
    gl::BufferData(GL_ARRAY_BUFFER_, static_cast<GLsizeiptr>(vbuf.size() * sizeof(Vertex)),
                   vbuf.data(), GL_STATIC_DRAW_);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                             reinterpret_cast<void*>(offsetof(Vertex, r)));
    gl::EnableVertexAttribArray(1);
    gl::BindVertexArray(0);

    glEnable(GL_DEPTH_TEST);

    int width = 1280, height = 720;
    float angle = 0.0f;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
                width = event.window.data1;
                height = event.window.data2;
                glViewport(0, 0, width, height);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
            }
        }

        angle += 0.008f;

        Mat4 model_m = Mat4::identity();

        Mat4::Vec3 eye{
            center[0] + std::cos(angle) * orbit_distance,
            center[1] + orbit_distance * 0.35f,
            center[2] + std::sin(angle) * orbit_distance,
        };
        Mat4 view = Mat4::lookAt(eye, {center[0], center[1], center[2]}, {0.0f, 1.0f, 0.0f});
        Mat4 proj = Mat4::perspective(50.0f * 3.14159265f / 180.0f,
                                       static_cast<float>(width) / static_cast<float>(height),
                                       0.01f, 100.0f);
        Mat4 mvp = proj * view * model_m;

        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        gl::UseProgram(program);
        gl::UniformMatrix4fv(mvpLoc, 1, GL_FALSE, &mvp.m[0][0]);
        gl::BindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vbuf.size()));

        SDL_GL_SwapWindow(window);
    }

    gl::DeleteVertexArrays(1, &vao);
    gl::DeleteBuffers(1, &vbo);
    gl::DeleteProgram(program);
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
