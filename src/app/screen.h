// One screen of the application: a menu, a lobby, a race.
//
// WHY THIS EXISTS. Until 2026-09-22 `openbigred.cpp` was a single 1 230-line `main()` that
// loaded a level at line 161 and opened the window at line 375. Nothing could be drawn before a
// level existed, no level could be swapped without restarting the process, and every GL handle
// was a local freed only by process exit. A multiplayer lobby has to run WITH a window and
// WITHOUT a level, so that order had to be broken.
//
// The contract is deliberately small:
//
//   enter()        acquire everything this screen needs; false means "could not", and the App
//                  pops it again rather than showing a broken screen.
//   handle_event() one SDL event. Return true if it was consumed.
//   update()       called once per frame with the real elapsed milliseconds. A screen that
//                  simulates does its OWN fixed-timestep accumulation in here - the App does
//                  not impose one, because only the race has a deterministic tick to keep.
//   draw()         render. `App::canvas()` says whether this screen draws into the 320x200
//                  menu canvas or at the window's own resolution.
//   exit()         release. Always called before the screen is destroyed, while the GL context
//                  is still current - which a destructor cannot promise.
//
// ⚠️ A screen must free every GL object it made, in exit(). The whole point of the stack is
// that screens come and go inside one process; a leak here is a leak per level loaded.
#pragma once

union SDL_Event;

namespace bigred {

class App;

class Screen {
public:
    virtual ~Screen() = default;

    virtual bool enter(App& app) { (void)app; return true; }
    virtual void exit(App& app) { (void)app; }

    // Called when this screen is the top of the stack again because what was above it was
    // popped. enter() does NOT run a second time - the screen never left - so anything it
    // shares with the screen above and wants back belongs here. The music is the first such
    // thing: a race plays the circuit's piece, and coming back to the menu must bring the
    // menu's own back.
    virtual void resumed(App& app) { (void)app; }

    virtual bool handle_event(App& app, const SDL_Event& e) { (void)app; (void)e; return false; }
    virtual void update(App& app, double dt_ms) { (void)app; (void)dt_ms; }
    virtual void draw(App& app) = 0;
    // After draw(), at the WINDOW's own resolution whatever the rung of the `+`/`-` ladder: what
    // must stay readable - a race's board. Not called for a screen that uses_canvas().
    virtual void draw_overlay(App& app) { (void)app; }

    // True for menus: the App wraps draw() in the 320x200 canvas and presents it in a 4:3 box.
    // False for the race, which renders at the player's chosen resolution, full frame.
    virtual bool uses_canvas() const { return true; }

    // A name for logs and for the window title. Screens are pushed and popped a lot once there
    // are menus, and "which screen am I on" is the first question of any bug report.
    virtual const char* name() const = 0;
};

} // namespace bigred
