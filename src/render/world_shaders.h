// The GLSL the world pass and the backdrop pass are drawn with, shared by both executables
// (map_preview and openbigred) so the game and the viewer can never drift apart visually.
//
#pragma once

#include <cstdlib>

namespace bigred {

// uCullSide for the decor's one-sided faces (TerrainVertex::cull). OBR_CULL_SIDE=1/-1/0 in the
// environment overrides it, to compare; 0 draws every face both ways, as before 2026-09-27.
constexpr float kDecorCullSideDefault = 1.0f;
inline float decor_cull_side() {
    if (const char* v = std::getenv("OBR_CULL_SIDE")) return static_cast<float>(std::atoi(v));
    return kDecorCullSideDefault;
}

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aLayer;
layout(location = 4) in float aHasTexture;
layout(location = 5) in vec3 aColorLit;
layout(location = 6) in float aLit; // 1 on tunnel geometry, 0 elsewhere (TerrainVertex::lit)
layout(location = 7) in float aCull; // 1 on a one-sided decor face (TerrainVertex::cull)
uniform mat4 uMVP;
uniform vec3 uCamPos;      // render-space camera position, for the distance shading below
uniform float uShadeRaw;   // distance at which the shading level reaches 0 (raw colour)
uniform float uShadeSpan;  // width of the ramp from full-bright to raw
out vec3 vColor;
out vec3 vColorLit;
out vec2 vUV;
out float vLayer;
out float vHasTexture;
out float vShade;
out float vCull;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vCull = aCull;
    vColor = aColor;
    vColorLit = aColorLit;
    vUV = aUV;
    vLayer = aLayer;
    vHasTexture = aHasTexture;
    // The original's own curve, from its depth-shading pass: level = ((cells-2)*0x200 - depth) >> 4,
    // clamped to 0..255. Two differences, both deliberate:
    // it takes depth per OBJECT along the view axis, we take it per VERTEX as a plain radial
    // distance; and it lands on one of 256 palette rungs where we interpolate continuously.
    // Only where the vertex says so: since 2026-09-27 the lighting is for the TUNNELS alone.
    vShade = aLit * clamp((uShadeRaw - distance(aPos, uCamPos)) / max(uShadeSpan, 1e-4), 0.0, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec3 vColor;
in vec3 vColorLit;
in vec2 vUV;
in float vLayer;
in float vHasTexture;
in float vShade;
in float vCull;
uniform sampler2DArray uAtlas;
// Which side of a one-sided face is NOT drawn: +1 the GL front, -1 the back. The original culls
// every face; what we call front depends on the mirrors between its axes and ours.
uniform float uCullSide;
uniform float uAtlasLayers; // the atlas array holds each layer TWICE: raw at L, lit at L + this
uniform float uShadeStrength; // 0 = no lighting at all, 1 = the full walk up the ramp
uniform float uShadeDarkCeil; // only colours darker than this luminance are brightened
out vec4 FragColor;
void main() {
    // A one-sided face seen from its hidden side does not exist - the original's backface
    // culling, per face, so recto/verso pairs keep being drawn as they were.
    if (vCull > 0.5 && (gl_FrontFacing ? uCullSide > 0.0 : uCullSide < 0.0)) discard;
    vec4 texel = texture(uAtlas, vec3(vUV, vLayer));
    // Palette index 0 decodes to alpha 0 (the .TM chroma key - see texture.cpp). Discard those
    // texels outright rather than blending, so cut-out sprites (banners, clouds) show the scene
    // behind them instead of a black rectangle, with no draw-order/alpha-sorting needed.
    // The lit copy is the same .TM through a brighter palette, so its index-0 mask is identical
    // and the raw layer's alpha decides for both.
    if (vHasTexture > 0.5 && texel.a < 0.5) discard;
    vec4 texelLit = texture(uAtlas, vec3(vUV, vLayer + uAtlasLayers));
    vec3 raw = mix(vColor, texel.rgb, vHasTexture);
    vec3 lit = mix(vColorLit, texelLit.rgb, vHasTexture);
    // Restricted to the DARK end of the ramps, deliberately - user call, 2026-09-14. Walking
    // every index up its ramp is what the original does, but it brightens 184 of 256 indices and
    // the whole near field washes out: faithful to the 1996 rasterizer, ugly on a modern one
    // with smooth gradients and no dithering. What was actually broken is narrower than that -
    // an index sitting on the darkest rung renders as flat black (the tunnels, index 31), which
    // is a colour the original never showed up close. So the correction is applied where it is
    // a correction, and faded out over anything already bright enough to read.
    float lum = dot(raw, vec3(0.299, 0.587, 0.114));
    float gate = 1.0 - smoothstep(0.0, max(uShadeDarkCeil, 1e-4), lum);
    FragColor = vec4(mix(raw, lit, vShade * gate * uShadeStrength), 1.0);
}
)";

// The backdrop, drawn before anything else. A full-screen triangle whose fragment shader
// reconstructs each pixel's view ray, takes its elevation above the horizon, and looks that up
// in the original's own band table (render/sky.h).
//
// The original draws hard-edged bands, because it is filling an 8-bit buffer with palette
// indices and has nothing to interpolate with. We interpolate between the same control points
// instead: same colours, same angles, same overall look, minus the banding. That is the
// deliberate "reproduce the result, not the technique" call. A
// future legacy render mode wants the hard steps back, so keep the table's angles exact.
constexpr const char* kSkyVertexShader = R"(#version 330 core
out vec2 vNdc;
void main() {
    // Fullscreen triangle from gl_VertexID alone - no vertex buffer, no attributes.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vNdc = p * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
)";

constexpr const char* kSkyFragmentShader = R"(#version 330 core
in vec2 vNdc;
uniform vec3 uForward;
uniform vec3 uRight;
uniform vec3 uUp;
uniform float uTanHalfFov;
uniform float uAspect;
uniform vec3 uSkyColor[16];
uniform float uSkyAngle[16];
uniform vec3 uGroundColor[2];
uniform float uGroundAngle[2];
out vec4 FragColor;

void main() {
    vec3 ray = normalize(uForward
                         + uRight * (vNdc.x * uTanHalfFov * uAspect)
                         + uUp    * (vNdc.y * uTanHalfFov));
    float elev = degrees(asin(clamp(ray.y, -1.0, 1.0)));

    vec3 c;
    if (elev >= 0.0) {
        // Angles descend to 0 at the horizon, so walk from the outermost band inwards and
        // interpolate against the previous control point.
        c = uSkyColor[0];
        for (int i = 0; i < 16; ++i) {
            if (elev >= uSkyAngle[i]) {
                if (i == 0) { c = uSkyColor[0]; }
                else {
                    float t = (elev - uSkyAngle[i]) / max(uSkyAngle[i - 1] - uSkyAngle[i], 1e-4);
                    c = mix(uSkyColor[i], uSkyColor[i - 1], clamp(t, 0.0, 1.0));
                }
                break;
            }
        }
    } else {
        float d = -elev;
        if (d >= uGroundAngle[0]) {
            c = uGroundColor[0];
        } else {
            float t = d / max(uGroundAngle[0] - uGroundAngle[1], 1e-4);
            c = mix(uGroundColor[1], uGroundColor[0], clamp(t, 0.0, 1.0));
        }
    }
    FragColor = vec4(c, 1.0);
}
)";

} // namespace bigred
