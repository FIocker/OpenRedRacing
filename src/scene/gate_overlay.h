// The checkpoint ring, drawn.
//
// `MAP.CSE`'s gates are invisible in the game: they are what the engine uses to know where a
// car is, and nothing about them is on screen. That makes them impossible to check by eye -
// and "are the rings where I think they are?" is exactly the question that came up the first
// time the checkpoint beep went missing. This turns the ring into geometry.
//
// It is a DIAGNOSTIC, and it lives in `map_preview` alone: the game shows the world, not its
// own bookkeeping (see openbigred.cpp's header). Everything here emits GL_LINES vertices in
// the same `TerrainVertex` layout and the same render space as the world mesh, so it draws
// with the world program and needs no shader of its own.
//
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "../formats/terrain.h"  // TerrainVertex
#include "../sim/race.h"         // Gate
#include "../sim/vehicle.h"      // SimTerrain

namespace bigred {

// What the ring is drawn with. The defaults are in RAW WORLD UNITS, like the gate table itself.
// ⚠️ The vocabulary follows race.h: a gate is a slab, `length` is across the track.
struct GateOverlayOptions {
    // How far above the ground the outline floats. A line laid exactly on the terrain z-fights
    // with it; 60 units is under a tenth of a car's height, so the outline still reads as
    // being on the road.
    float lift = 60.0f;
    // The corner posts, so a gate is visible from a car's eye height and not only from above.
    float post_height = 1200.0f;
    // How many segments an edge is cut into to follow the ground. An edge can be 10 000 units
    // long and cross several terrain cells - drawn as one straight line it disappears into the
    // hill it spans.
    int edge_steps = 10;
    // Draw the exit edge and the sides too, not just the entry edge and the posts. Off makes a
    // dense ring (69 gates on WATER2) much easier to read.
    bool full_trapezoid = true;
};

// The whole ring. `terrain` supplies the ground height under each point and may be invalid, in
// which case everything is drawn at y = 0 and only the shape in plan is meaningful.
//
// Each gate is drawn as the slab of road it is: the two faces a car crosses, a mast at each
// verge of the ENTRY face, the across-track middle line, and a short mast at the point a
// respawn puts a car (the middle of the road).
//
// The colours say what each gate IS, and they are the whole point of the picture:
//   * gate 0, the start/finish line - GREEN
//   * a gate a `CHECKP` line is attributed to (where ALT puts a car back) - ORANGE
//   * a gate the level marked `unsafe` - VIOLET
//   * every other gate of the ring - BLUE
std::vector<TerrainVertex> build_gate_overlay(const std::vector<Gate>& gates,
                                              const SimTerrain& terrain, int map_w,
                                              float render_scale,
                                              const GateOverlayOptions& opt = GateOverlayOptions());

// The `CHECKP` lines themselves - what actually FIRES a checkpoint - in YELLOW: the outline of
// the invisible box the car has to touch (2048 across, 100 thick), a mast at each end and a
// taller one in the middle carrying one crossbar per ordinal, so the order reads off the
// screen (none on the start/finish line). Several boxes with the same ordinal are pieces of one
// line. See sim/race.h, CheckLine.
std::vector<TerrainVertex> build_check_line_overlay(
    const std::vector<CheckLine>& lines, const SimTerrain& terrain, int map_w,
    float render_scale, const GateOverlayOptions& opt = GateOverlayOptions());

} // namespace bigred
