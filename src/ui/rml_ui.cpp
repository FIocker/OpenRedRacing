#include "rml_ui.h"

#include <RmlUi/Core.h>

#include <SDL.h>

#include <cstdio>

namespace bigred {
namespace {

// SDL to RmlUi, for the keys a menu actually uses: move, choose, go back, and enough of the
// keyboard for a text field. Anything not here simply is not forwarded - a menu is not a
// terminal, and an unmapped key must do nothing rather than something surprising.
Rml::Input::KeyIdentifier to_rml_key(SDL_Keycode k) {
    using namespace Rml::Input;
    if (k >= SDLK_a && k <= SDLK_z)
        return static_cast<KeyIdentifier>(KI_A + (k - SDLK_a));
    if (k >= SDLK_0 && k <= SDLK_9)
        return static_cast<KeyIdentifier>(KI_0 + (k - SDLK_0));
    switch (k) {
    case SDLK_SPACE: return KI_SPACE;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return KI_RETURN;
    case SDLK_ESCAPE: return KI_ESCAPE;
    case SDLK_BACKSPACE: return KI_BACK;
    case SDLK_DELETE: return KI_DELETE;
    case SDLK_TAB: return KI_TAB;
    case SDLK_UP: return KI_UP;
    case SDLK_DOWN: return KI_DOWN;
    case SDLK_LEFT: return KI_LEFT;
    case SDLK_RIGHT: return KI_RIGHT;
    case SDLK_HOME: return KI_HOME;
    case SDLK_END: return KI_END;
    case SDLK_PAGEUP: return KI_PRIOR;
    case SDLK_PAGEDOWN: return KI_NEXT;
    default: return KI_UNKNOWN;
    }
}

int to_rml_modifiers(uint16_t mod) {
    int m = 0;
    if (mod & KMOD_SHIFT) m |= Rml::Input::KM_SHIFT;
    if (mod & KMOD_CTRL) m |= Rml::Input::KM_CTRL;
    if (mod & KMOD_ALT) m |= Rml::Input::KM_ALT;
    return m;
}

} // namespace

RmlUi::~RmlUi() { shutdown(); }

bool RmlUi::init(const std::string& game_dir) {
    if (initialised_) return true;
    if (!renderer_.init(game_dir)) return false;

    Rml::SetRenderInterface(&renderer_);
    Rml::SetSystemInterface(&system_);
    Rml::SetFontEngineInterface(&fonts_);
    if (!Rml::Initialise()) {
        std::fprintf(stderr, "RmlUi: Initialise() failed - there will be no menus.\n");
        renderer_.release();
        return false;
    }
    // A menu with no text is ugly but diagnosable; a menu that refuses to open is not. So a
    // missing alphabet is not a failure here.
    fonts_.load(game_dir, renderer_);
    initialised_ = true;
    return true;
}

void RmlUi::shutdown() {
    if (!initialised_) return;
    Rml::Shutdown();
    initialised_ = false;
    // ⚠️ AFTER Rml::Shutdown(), never before: shutting down releases every document, and each
    // release calls back into the renderer to free its geometry and textures.
    renderer_.release();
}

Rml::Context* RmlUi::create_context(const char* name, int width, int height) {
    if (!initialised_) return nullptr;
    renderer_.set_viewport(width, height);
    return Rml::CreateContext(name, Rml::Vector2i(width, height));
}

void RmlUi::remove_context(Rml::Context* context) {
    if (context) Rml::RemoveContext(context->GetName());
}

bool RmlUi::handle_event(Rml::Context* context, const SDL_Event& e, float cx, float cy) {
    if (!context) return false;
    switch (e.type) {
    case SDL_MOUSEMOTION:
        return context->ProcessMouseMove(static_cast<int>(cx), static_cast<int>(cy),
                                         to_rml_modifiers(SDL_GetModState()));
    case SDL_MOUSEBUTTONDOWN:
        // ⚠️ THE POINTER IS PLACED FIRST. A context under another screen sees no motion while
        // it is covered, so its hover is wherever the pointer was when it was left. Coming back
        // to it and clicking without moving hit THAT element: BACK on NETWORK popped to
        // MULTIPLAYER, whose BACK sits at the same place, and a second click there went back
        // into NETWORK - its hover was still the row that had opened it.
        context->ProcessMouseMove(static_cast<int>(cx), static_cast<int>(cy),
                                  to_rml_modifiers(SDL_GetModState()));
        // RmlUi numbers buttons 0 = left, 1 = right, 2 = middle; SDL numbers them from 1 in a
        // different order.
        return context->ProcessMouseButtonDown(e.button.button == SDL_BUTTON_LEFT    ? 0
                                               : e.button.button == SDL_BUTTON_RIGHT ? 1
                                                                                     : 2,
                                               to_rml_modifiers(SDL_GetModState()));
    case SDL_MOUSEBUTTONUP:
        return context->ProcessMouseButtonUp(e.button.button == SDL_BUTTON_LEFT    ? 0
                                             : e.button.button == SDL_BUTTON_RIGHT ? 1
                                                                                   : 2,
                                             to_rml_modifiers(SDL_GetModState()));
    case SDL_MOUSEWHEEL:
        return context->ProcessMouseWheel(static_cast<float>(-e.wheel.y),
                                          to_rml_modifiers(SDL_GetModState()));
    case SDL_KEYDOWN: {
        const Rml::Input::KeyIdentifier key = to_rml_key(e.key.keysym.sym);
        if (key == Rml::Input::KI_UNKNOWN) return false;
        return context->ProcessKeyDown(key, to_rml_modifiers(e.key.keysym.mod));
    }
    case SDL_KEYUP: {
        const Rml::Input::KeyIdentifier key = to_rml_key(e.key.keysym.sym);
        if (key == Rml::Input::KI_UNKNOWN) return false;
        return context->ProcessKeyUp(key, to_rml_modifiers(e.key.keysym.mod));
    }
    case SDL_TEXTINPUT:
        return context->ProcessTextInput(Rml::String(e.text.text));
    default:
        return false;
    }
}

} // namespace bigred
