// Minimal column-major 4x4 float matrix helpers (identity/perspective/lookAt/multiply) -
// hand-rolled to avoid pulling in a math library dependency for this small a need. Standard
// OpenGL column-major layout: mat4::m[col][row], uploaded to GLSL as-is (transpose=GL_FALSE).
#pragma once

#include <cmath>

struct Mat4 {
    float m[4][4] = {};

    static Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    static Mat4 perspective(float fovy_radians, float aspect, float near_z, float far_z) {
        Mat4 r;
        float f = 1.0f / std::tan(fovy_radians * 0.5f);
        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = (far_z + near_z) / (near_z - far_z);
        r.m[2][3] = -1.0f;
        r.m[3][2] = (2.0f * far_z * near_z) / (near_z - far_z);
        return r;
    }

    struct Vec3 { float x, y, z; };

    static Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    static Vec3 cross(Vec3 a, Vec3 b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static Vec3 normalize(Vec3 v) {
        float len = std::sqrt(dot(v, v));
        if (len < 1e-6f) return {0, 0, 0};
        return {v.x / len, v.y / len, v.z / len};
    }

    static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
        Vec3 f = normalize(sub(center, eye));
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);

        Mat4 r = identity();
        r.m[0][0] = s.x; r.m[1][0] = s.y; r.m[2][0] = s.z;
        r.m[0][1] = u.x; r.m[1][1] = u.y; r.m[2][1] = u.z;
        r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
        r.m[3][0] = -dot(s, eye);
        r.m[3][1] = -dot(u, eye);
        r.m[3][2] = dot(f, eye);
        return r;
    }

    // this * other (applies `other` first, then `this` - standard OpenGL convention)
    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += m[k][row] * o.m[col][k];
                r.m[col][row] = sum;
            }
        }
        return r;
    }
};
