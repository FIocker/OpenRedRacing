#include "gl_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace gl {

void (APIENTRY* GenBuffers)(GLsizei, GLuint*) = nullptr;
void (APIENTRY* BindBuffer)(GLenum, GLuint) = nullptr;
void (APIENTRY* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
void (APIENTRY* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
void (APIENTRY* GenVertexArrays)(GLsizei, GLuint*) = nullptr;
void (APIENTRY* BindVertexArray)(GLuint) = nullptr;
void (APIENTRY* DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
void (APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
void (APIENTRY* EnableVertexAttribArray)(GLuint) = nullptr;

GLuint (APIENTRY* CreateShader)(GLenum) = nullptr;
void (APIENTRY* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (APIENTRY* CompileShader)(GLuint) = nullptr;
void (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
void (APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (APIENTRY* DeleteShader)(GLuint) = nullptr;
GLuint (APIENTRY* CreateProgram)(void) = nullptr;
void (APIENTRY* AttachShader)(GLuint, GLuint) = nullptr;
void (APIENTRY* LinkProgram)(GLuint) = nullptr;
void (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void (APIENTRY* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (APIENTRY* UseProgram)(GLuint) = nullptr;
void (APIENTRY* DeleteProgram)(GLuint) = nullptr;
GLint (APIENTRY* GetUniformLocation)(GLuint, const GLchar*) = nullptr;
void (APIENTRY* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
void (APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
void (APIENTRY* Uniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
void (APIENTRY* Uniform1f)(GLint, GLfloat) = nullptr;
void (APIENTRY* Uniform1i)(GLint, GLint) = nullptr;
void (APIENTRY* TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
void (APIENTRY* TexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;
void (APIENTRY* ActiveTexture)(GLenum) = nullptr;
void (APIENTRY* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
void (APIENTRY* BindFramebuffer)(GLenum, GLuint) = nullptr;
void (APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
void (APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
GLenum (APIENTRY* CheckFramebufferStatus)(GLenum) = nullptr;
void (APIENTRY* GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
void (APIENTRY* BindRenderbuffer)(GLenum, GLuint) = nullptr;
void (APIENTRY* DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
void (APIENTRY* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
void (APIENTRY* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;

namespace {

template <typename Fn>
bool get(const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(wglGetProcAddress(name)));
    if (!out) {
        std::fprintf(stderr, "gl::load: failed to resolve %s\n", name);
        return false;
    }
    return true;
}

} // namespace

bool load() {
    bool ok = true;
    ok &= get("glGenBuffers", GenBuffers);
    ok &= get("glBindBuffer", BindBuffer);
    ok &= get("glBufferData", BufferData);
    ok &= get("glDeleteBuffers", DeleteBuffers);
    ok &= get("glGenVertexArrays", GenVertexArrays);
    ok &= get("glBindVertexArray", BindVertexArray);
    ok &= get("glDeleteVertexArrays", DeleteVertexArrays);
    ok &= get("glVertexAttribPointer", VertexAttribPointer);
    ok &= get("glEnableVertexAttribArray", EnableVertexAttribArray);

    ok &= get("glCreateShader", CreateShader);
    ok &= get("glShaderSource", ShaderSource);
    ok &= get("glCompileShader", CompileShader);
    ok &= get("glGetShaderiv", GetShaderiv);
    ok &= get("glGetShaderInfoLog", GetShaderInfoLog);
    ok &= get("glDeleteShader", DeleteShader);
    ok &= get("glCreateProgram", CreateProgram);
    ok &= get("glAttachShader", AttachShader);
    ok &= get("glLinkProgram", LinkProgram);
    ok &= get("glGetProgramiv", GetProgramiv);
    ok &= get("glGetProgramInfoLog", GetProgramInfoLog);
    ok &= get("glUseProgram", UseProgram);
    ok &= get("glDeleteProgram", DeleteProgram);
    ok &= get("glGetUniformLocation", GetUniformLocation);
    ok &= get("glUniformMatrix4fv", UniformMatrix4fv);
    ok &= get("glUniform2f", Uniform2f);
    ok &= get("glUniform3f", Uniform3f);
    ok &= get("glUniform1f", Uniform1f);
    ok &= get("glUniform1i", Uniform1i);
    ok &= get("glTexImage3D", TexImage3D);
    ok &= get("glTexSubImage3D", TexSubImage3D);
    ok &= get("glActiveTexture", ActiveTexture);

    ok &= get("glGenFramebuffers", GenFramebuffers);
    ok &= get("glBindFramebuffer", BindFramebuffer);
    ok &= get("glDeleteFramebuffers", DeleteFramebuffers);
    ok &= get("glFramebufferTexture2D", FramebufferTexture2D);
    ok &= get("glCheckFramebufferStatus", CheckFramebufferStatus);
    ok &= get("glGenRenderbuffers", GenRenderbuffers);
    ok &= get("glBindRenderbuffer", BindRenderbuffer);
    ok &= get("glDeleteRenderbuffers", DeleteRenderbuffers);
    ok &= get("glRenderbufferStorage", RenderbufferStorage);
    ok &= get("glFramebufferRenderbuffer", FramebufferRenderbuffer);

    return ok;
}

} // namespace gl
