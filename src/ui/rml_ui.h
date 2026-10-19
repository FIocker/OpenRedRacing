// The interface runtime: RmlUi initialised once, with our three interfaces plugged into it.
//
//
// ⚠️ ONE PER PROCESS, and the App owns it - like the audio device, and for the same reason. A
// menu screen comes and goes; `Rml::Initialise()` must not, or every screen change would throw
// away the parsed stylesheets and rebuild the font atlas out of the game's archives.
//
// It is created LAZILY, on the first menu: it needs a GL context and the game directory, and it
// costs nothing to a session that only ever races.
#pragma once

#include <string>

namespace Rml {
class Context;
}
union SDL_Event;

#include "ui/rml_font_engine.h"
#include "ui/rml_render_interface.h"
#include "ui/rml_system_interface.h"

namespace bigred {

class RmlUi {
public:
    ~RmlUi();

    // Idempotent. False means no menu can be drawn - the shader did not compile, or RmlUi
    // itself refused to start. A missing ALWAYS.WAP is NOT a failure: the menus then draw
    // without text, which is at least visible evidence of what is wrong.
    bool init(const std::string& game_dir);
    bool ready() const { return initialised_; }
    void shutdown();

    // A context is a screenful of documents. Its size is the CANVAS', not the window's
    // (app/presentation.h).
    Rml::Context* create_context(const char* name, int width, int height);
    void remove_context(Rml::Context* context);

    RmlRenderInterface& renderer() { return renderer_; }

    // Translates one SDL event and feeds it to `context`. `cx`/`cy` are the pointer in CANVAS
    // coordinates, which the caller gets from App::mouse_canvas - RmlUi must never be handed
    // window coordinates or every click lands in the wrong place once there is a letterbox.
    // Returns true when RmlUi consumed the event.
    bool handle_event(Rml::Context* context, const SDL_Event& e, float cx, float cy);

private:
    RmlRenderInterface renderer_;
    RmlSystemInterface system_;
    RmlFontEngine fonts_;
    bool initialised_ = false;
};

} // namespace bigred
