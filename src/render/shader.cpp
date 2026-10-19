#include "shader.h"

#include <cstdio>
#include <vector>

namespace {

GLuint compile_one(GLenum type, const char* src) {
    GLuint shader = gl::CreateShader(type);
    gl::ShaderSource(shader, 1, &src, nullptr);
    gl::CompileShader(shader);
    GLint ok = 0;
    gl::GetShaderiv(shader, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        GLint len = 0;
        gl::GetShaderiv(shader, 0x8B84 /*GL_INFO_LOG_LENGTH*/, &len);
        std::vector<char> log(len > 0 ? static_cast<size_t>(len) : 1);
        gl::GetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::fprintf(stderr, "shader compile error: %s\n", log.data());
        gl::DeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

GLuint Shader::compile(const char* vertex_src, const char* fragment_src) {
    GLuint vs = compile_one(GL_VERTEX_SHADER_, vertex_src);
    GLuint fs = compile_one(GL_FRAGMENT_SHADER_, fragment_src);
    if (!vs || !fs) return 0;

    GLuint program = gl::CreateProgram();
    gl::AttachShader(program, vs);
    gl::AttachShader(program, fs);
    gl::LinkProgram(program);

    GLint ok = 0;
    gl::GetProgramiv(program, GL_LINK_STATUS_, &ok);
    if (!ok) {
        GLint len = 0;
        gl::GetProgramiv(program, 0x8B84 /*GL_INFO_LOG_LENGTH*/, &len);
        std::vector<char> log(len > 0 ? static_cast<size_t>(len) : 1);
        gl::GetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::fprintf(stderr, "shader link error: %s\n", log.data());
        gl::DeleteProgram(program);
        program = 0;
    }

    gl::DeleteShader(vs);
    gl::DeleteShader(fs);
    return program;
}
