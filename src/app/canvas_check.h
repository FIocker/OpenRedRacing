// `openbigred --canvas <SCREEN>` - the screen that exists to be LOOKED AT.
//
// It draws one original menu background through the whole menu path: the `.SCN` unpacked as the
// archive it is, its `.RAW` decoded with its own palette, laid 1:1 into the 320x200 canvas, and
// presented in a centred 4:3 box. Nothing else. It is the only way to judge the decision on
// the two display spaces - whether a 1996 menu stretched back
// to 4:3 looks right - and that is a judgement for the user, not for a test.
//
// It is a diagnostic entry point, in the spirit of `--list` and `-DEBUG`, and it touches nothing
// the race uses.
#pragma once

#include <string>

#include "app/screen.h"
#include "render/hud.h"
#include "ui/background.h"

namespace bigred {

class CanvasCheck : public Screen {
public:
    explicit CanvasCheck(std::string scn) : scn_(std::move(scn)) {}

    bool enter(App& app) override;
    void exit(App& app) override;
    bool handle_event(App& app, const SDL_Event& e) override;
    void draw(App& app) override;
    const char* name() const override { return "canvas-check"; }

private:
    std::string scn_;
    Background background_;
    Hud hud_;          // draws the frame, the marker and the readout, in canvas pixels
    float mx_ = -1, my_ = -1;  // the pointer, in canvas coordinates
    bool in_canvas_ = false;
};

} // namespace bigred
