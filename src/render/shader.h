// Tiny helper to compile/link a GL 3.3 core vertex+fragment shader pair.
#pragma once

#include "gl_loader.h"

#include <string>

class Shader {
public:
    // Returns a linked program, or 0 on failure (compile/link errors are printed to stderr).
    static GLuint compile(const char* vertex_src, const char* fragment_src);
};
