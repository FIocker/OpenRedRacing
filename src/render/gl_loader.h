// Minimal OpenGL 3.3 core function-pointer loader (Windows/WGL only, via wglGetProcAddress).
// Deliberately hand-written instead of pulling in GLAD/glbinding: only the small function set
// this engine actually uses so far is declared here. This
// was explicitly deferred until real rendering (shaders/VBOs) was needed; that's now.
#pragma once

#include <GL/gl.h> // core 1.1 types (GLuint, GLenum, ...) and the handful of 1.1 entry points
                   // (glClear, glViewport, glDrawArrays, ...) that don't need loading.

// GL 1.5+/2.0+/3.0+ types and constants not in the old (GL 1.1) <GL/gl.h> header MinGW ships.
using GLsizeiptr = ptrdiff_t;
using GLchar = char;

constexpr GLenum GL_ARRAY_BUFFER_ = 0x8892;
// Indexed geometry: the interface layer draws what RmlUi hands it, which is indexed.
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER_ = 0x8893;
constexpr GLenum GL_STATIC_DRAW_ = 0x88E4;
constexpr GLenum GL_STREAM_DRAW_ = 0x88E0; // re-uploaded every frame - the billboard buffer
constexpr GLenum GL_VERTEX_SHADER_ = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER_ = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS_ = 0x8B81;
constexpr GLenum GL_LINK_STATUS_ = 0x8B82;
constexpr GLenum GL_TEXTURE_2D_ARRAY_ = 0x8C1A;
constexpr GLenum GL_CLAMP_TO_EDGE_ = 0x812F;
constexpr GLenum GL_TEXTURE0_ = 0x84C0;

// Framebuffer objects (GL 3.0) - the menus' 320x200 offscreen canvas.
constexpr GLenum GL_FRAMEBUFFER_ = 0x8D40;
constexpr GLenum GL_COLOR_ATTACHMENT0_ = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;
// A depth attachment, for the world rendered at a reduced resolution (the + / - keys).
constexpr GLenum GL_RENDERBUFFER_ = 0x8D41;
constexpr GLenum GL_DEPTH_ATTACHMENT_ = 0x8D00;
constexpr GLenum GL_DEPTH_COMPONENT24_ = 0x81A6;

namespace gl {

// Buffers / vertex arrays (GL 1.5 / 3.0)
extern void (APIENTRY* GenBuffers)(GLsizei n, GLuint* buffers);
extern void (APIENTRY* BindBuffer)(GLenum target, GLuint buffer);
extern void (APIENTRY* BufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
extern void (APIENTRY* DeleteBuffers)(GLsizei n, const GLuint* buffers);
extern void (APIENTRY* GenVertexArrays)(GLsizei n, GLuint* arrays);
extern void (APIENTRY* BindVertexArray)(GLuint array);
extern void (APIENTRY* DeleteVertexArrays)(GLsizei n, const GLuint* arrays);
extern void (APIENTRY* VertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                             GLsizei stride, const void* pointer);
extern void (APIENTRY* EnableVertexAttribArray)(GLuint index);

// Shaders / programs (GL 2.0)
extern GLuint (APIENTRY* CreateShader)(GLenum type);
extern void (APIENTRY* ShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
extern void (APIENTRY* CompileShader)(GLuint shader);
extern void (APIENTRY* GetShaderiv)(GLuint shader, GLenum pname, GLint* params);
extern void (APIENTRY* GetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
extern void (APIENTRY* DeleteShader)(GLuint shader);
extern GLuint (APIENTRY* CreateProgram)(void);
extern void (APIENTRY* AttachShader)(GLuint program, GLuint shader);
extern void (APIENTRY* LinkProgram)(GLuint program);
extern void (APIENTRY* GetProgramiv)(GLuint program, GLenum pname, GLint* params);
extern void (APIENTRY* GetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
extern void (APIENTRY* UseProgram)(GLuint program);
extern void (APIENTRY* DeleteProgram)(GLuint program);
extern GLint (APIENTRY* GetUniformLocation)(GLuint program, const GLchar* name);
extern void (APIENTRY* UniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
extern void (APIENTRY* Uniform2f)(GLint location, GLfloat v0, GLfloat v1);
extern void (APIENTRY* Uniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
extern void (APIENTRY* Uniform1f)(GLint location, GLfloat v0);
extern void (APIENTRY* Uniform1i)(GLint location, GLint v0);

// Textures (GL 1.2's TexImage3D, used here for a GL_TEXTURE_2D_ARRAY of terrain atlases)
extern void (APIENTRY* TexImage3D)(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                    GLsizei height, GLsizei depth, GLint border, GLenum format,
                                    GLenum type, const void* pixels);
// One layer of that array rewritten in place - the cabin view's dashboard, every frame.
extern void (APIENTRY* TexSubImage3D)(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                       GLint zoffset, GLsizei width, GLsizei height,
                                       GLsizei depth, GLenum format, GLenum type,
                                       const void* pixels);
extern void (APIENTRY* ActiveTexture)(GLenum texture);

// Framebuffer objects (GL 3.0): one colour attachment for the menu canvas, plus a depth
// renderbuffer for the world - which the menus do not need (they are 2D) but the reduced-
// resolution render of the race does. No multisampling either way: it is pixel art.
extern void (APIENTRY* GenFramebuffers)(GLsizei n, GLuint* framebuffers);
extern void (APIENTRY* BindFramebuffer)(GLenum target, GLuint framebuffer);
extern void (APIENTRY* DeleteFramebuffers)(GLsizei n, const GLuint* framebuffers);
extern void (APIENTRY* FramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget,
                                              GLuint texture, GLint level);
extern GLenum (APIENTRY* CheckFramebufferStatus)(GLenum target);
extern void (APIENTRY* GenRenderbuffers)(GLsizei n, GLuint* renderbuffers);
extern void (APIENTRY* BindRenderbuffer)(GLenum target, GLuint renderbuffer);
extern void (APIENTRY* DeleteRenderbuffers)(GLsizei n, const GLuint* renderbuffers);
extern void (APIENTRY* RenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width,
                                             GLsizei height);
extern void (APIENTRY* FramebufferRenderbuffer)(GLenum target, GLenum attachment,
                                                 GLenum renderbuffertarget, GLuint renderbuffer);

// Loads every pointer above via wglGetProcAddress. Must be called after an OpenGL context is
// current (e.g. right after SDL_GL_CreateContext). Returns false (and leaves an error on
// stderr) if any required function couldn't be resolved.
bool load();

} // namespace gl
