#include "gate_overlay.h"

#include <cmath>

#include "level_scene.h"  // kRenderScale's companions: kTerrainTexelWorldSize via terrain.h

namespace bigred {
namespace {

struct Rgb {
    float r, g, b;
};

// See the header: the colour IS the information.
constexpr Rgb kStartLine{0.20f, 1.00f, 0.35f};
constexpr Rgb kSounding{1.00f, 0.45f, 0.05f};
constexpr Rgb kUnsafe{0.85f, 0.25f, 1.00f};
constexpr Rgb kPlain{0.15f, 0.55f, 1.00f};
constexpr Rgb kGantry{1.00f, 0.95f, 0.15f};
// The middle line that shows which way the gate is facing - deliberately dimmer than the
// outline, so a ring of 69 gates does not turn into a solid mat of colour.
constexpr float kAxisDim = 0.55f;

// Emits into RENDER space, the same transform the terrain and the decor use: world X mirrored
// about (map_w - 1) * 512, and +Y flipped because the game points it DOWN.
// ⚠️ the `- 1` is not optional: without it everything lands one grid cell off.
struct Emitter {
    std::vector<TerrainVertex>* out;
    const SimTerrain* terrain;
    float mirror_axis;
    float scale;
    float lift;

    // World y (game convention, +Y down) of the ground under a raw world (x, z).
    float ground(int32_t x, int32_t z) const {
        if (!terrain || !terrain->valid()) return 0.0f;
        return static_cast<float>(terrain->height_fx(x << 9, z << 9)) / 512.0f;
    }

    TerrainVertex vertex(int32_t x, int32_t z, float y_down, const Rgb& c) const {
        TerrainVertex v{};
        v.x = (mirror_axis - static_cast<float>(x)) * scale;
        v.y = -y_down * scale;
        v.z = static_cast<float>(z) * scale;
        v.r = c.r;
        v.g = c.g;
        v.b = c.b;
        // Same colour at both ends of the shading ramp: a diagnostic must not change colour
        // with distance - the same call build_marker_mesh makes.
        v.br = c.r;
        v.bg = c.g;
        v.bb = c.b;
        v.u = v.v = 0.0f;
        v.layer = 0.0f;
        v.has_texture = 0.0f;
        return v;
    }

    // A segment lying ON the ground, cut into `steps` pieces so it follows the terrain.
    void ground_line(int32_t x0, int32_t z0, int32_t x1, int32_t z1, int steps, const Rgb& c) {
        if (steps < 1) steps = 1;
        for (int i = 0; i < steps; ++i) {
            const float t0 = static_cast<float>(i) / steps;
            const float t1 = static_cast<float>(i + 1) / steps;
            const int32_t ax = x0 + static_cast<int32_t>((x1 - x0) * t0);
            const int32_t az = z0 + static_cast<int32_t>((z1 - z0) * t0);
            const int32_t bx = x0 + static_cast<int32_t>((x1 - x0) * t1);
            const int32_t bz = z0 + static_cast<int32_t>((z1 - z0) * t1);
            out->push_back(vertex(ax, az, ground(ax, az) - lift, c));
            out->push_back(vertex(bx, bz, ground(bx, bz) - lift, c));
        }
    }

    // A vertical mast at a point. +Y is DOWN in world units, so "up" is a subtraction.
    void post(int32_t x, int32_t z, float height, const Rgb& c) {
        const float g = ground(x, z);
        out->push_back(vertex(x, z, g - lift, c));
        out->push_back(vertex(x, z, g - lift - height, c));
    }
};

} // namespace

std::vector<TerrainVertex> build_gate_overlay(const std::vector<Gate>& gates,
                                              const SimTerrain& terrain, int map_w,
                                              float render_scale,
                                              const GateOverlayOptions& opt) {
    std::vector<TerrainVertex> out;
    if (gates.empty()) return out;
    out.reserve(gates.size() * 128);

    Emitter em{&out, &terrain,
               static_cast<float>(map_w - 1) * static_cast<float>(kTerrainTexelWorldSize),
               render_scale, opt.lift};

    for (size_t i = 0; i < gates.size(); ++i) {
        const Gate& g = gates[i];
        const Rgb c = (i == 0) ? kStartLine
                      : !g.safe ? kUnsafe
                      : g.checkpoint ? kSounding
                                     : kPlain;

        // A gate is a SLAB OF ROAD: `length` runs ACROSS the track and the depth runs along
        // it, tapering from one verge to the other (race.h). So the two faces a car crosses are
        // the ones at constant along-track, and they have to be drawn segment by segment,
        // because the depth changes with the across-track position - a face is a slanted line,
        // not a parallel one.
        const int32_t half_a = gate_half_depth_at(g, 0);
        const int32_t half_b = gate_half_depth_at(g, g.length);
        int32_t ax, az, bx, bz, cx, cz, dx, dz;
        gate_point(g, 0, -half_a, ax, az);        // entry face, anchor's verge
        gate_point(g, g.length, -half_b, bx, bz); // entry face, far verge
        gate_point(g, g.length, half_b, cx, cz);  // exit face, far verge
        gate_point(g, 0, half_a, dx, dz);         // exit face, anchor's verge

        // The ENTRY FACE is the one the beep fires on, so it is drawn whatever the options say,
        // and it carries the posts - which stand at the two VERGES, which is what makes the
        // gate read as a gate.
        em.ground_line(ax, az, bx, bz, opt.edge_steps, c);
        em.post(ax, az, opt.post_height, c);
        em.post(bx, bz, opt.post_height, c);
        if (opt.full_trapezoid) {
            em.ground_line(bx, bz, cx, cz, opt.edge_steps, c);
            em.ground_line(cx, cz, dx, dz, opt.edge_steps, c);
            em.ground_line(dx, dz, ax, az, opt.edge_steps, c);
        }

        // The across-track middle line, from verge to verge through the middle of the slab.
        // Its midpoint is exactly where a respawn puts a car, so it is the line to check
        // against the road.
        int32_t m0x, m0z, m1x, m1z;
        gate_point(g, 0, 0, m0x, m0z);
        gate_point(g, g.length, 0, m1x, m1z);
        const Rgb dim{c.r * kAxisDim, c.g * kAxisDim, c.b * kAxisDim};
        em.ground_line(m0x, m0z, m1x, m1z, opt.edge_steps, dim);
        // And a short mast at the RESPAWN POINT - the middle of the road, `length / 2` across.
        int32_t rx, rz;
        gate_point(g, g.length / 2, 0, rx, rz);
        em.post(rx, rz, opt.post_height * 0.6f, dim);
    }
    return out;
}

std::vector<TerrainVertex> build_check_line_overlay(const std::vector<CheckLine>& lines,
                                                    const SimTerrain& terrain, int map_w,
                                                    float render_scale,
                                                    const GateOverlayOptions& opt) {
    std::vector<TerrainVertex> out;
    if (lines.empty()) return out;
    out.reserve(lines.size() * 64);

    Emitter em{&out, &terrain,
               static_cast<float>(map_w - 1) * static_cast<float>(kTerrainTexelWorldSize),
               render_scale, opt.lift};

    for (const CheckLine& c : lines) {
        // The box as the race tests it: local +X is heading yaw + a quarter turn, local +Z is
        // heading yaw, and a heading h is the world direction (sin h, cos h). Display only, so
        // floats are fine here.
        const double a = static_cast<double>(c.yaw_bam16 & 0xFFFF) * 6.283185307179586 / 65536.0;
        const double xx = std::cos(a), xz = -std::sin(a);   // sin/cos of a + quarter turn
        const double zx = std::sin(a), zz = std::cos(a);
        auto at = [&](int32_t lx, int32_t lz, int32_t& wx, int32_t& wz) {
            wx = c.x + static_cast<int32_t>(std::lround(lx * xx + lz * zx));
            wz = c.z + static_cast<int32_t>(std::lround(lx * xz + lz * zz));
        };
        int32_t p[4][2];
        at(c.box_xmin, c.box_zmin, p[0][0], p[0][1]);
        at(c.box_xmax, c.box_zmin, p[1][0], p[1][1]);
        at(c.box_xmax, c.box_zmax, p[2][0], p[2][1]);
        at(c.box_xmin, c.box_zmax, p[3][0], p[3][1]);
        for (int k = 0; k < 4; ++k)
            em.ground_line(p[k][0], p[k][1], p[(k + 1) % 4][0], p[(k + 1) % 4][1],
                           opt.edge_steps, kGantry);
        // A mast at each end, and a taller one in the middle - one notch per ordinal, so the
        // ORDER can be read off the screen: ordinal 0 (the line) has none.
        em.post(p[0][0], p[0][1], opt.post_height, kGantry);
        em.post(p[1][0], p[1][1], opt.post_height, kGantry);
        em.post(c.x, c.z, opt.post_height * 2.0f, kGantry);
        const float g = em.ground(c.x, c.z);
        for (int k = 0; k < c.ordinal(); ++k) {
            const float h = g - opt.lift - opt.post_height * 2.0f + 150.0f * static_cast<float>(k);
            int32_t l0x, l0z, l1x, l1z;
            at(-200, 0, l0x, l0z);
            at(200, 0, l1x, l1z);
            out.push_back(em.vertex(l0x, l0z, h, kGantry));
            out.push_back(em.vertex(l1x, l1z, h, kGantry));
        }
    }
    return out;
}

} // namespace bigred
