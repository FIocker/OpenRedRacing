// A reusable base for a screen that is one RmlUi document: context/canvas lifecycle, and mouse +
// keyboard + gamepad unified into three verbs - move, activate, back.
//
//
// ⚠️ WHY THIS EXISTS NOW, BEFORE THE SECOND SCREEN THAT NEEDS IT: "le focus manette entre là,
// pas après" (the design doc, verbatim) - rule 6 of the same doc requires every screen to be
// fully usable on a gamepad, and retrofitting that onto documents already written costs far more
// than building it into the one base every later screen is written against. `quick_race_screen`
// predates this and is deliberately NOT ported onto it (see that file's header) - its bespoke
// Up/Down/Left/Right/click handling already works and a cosmetic unification is not worth the
// risk to code that is validated in-game.
//
// A subclass:
//   - implements document_source() (an RML string) and context_name() (must be unique among
//     screens that can be on the stack AT THE SAME TIME - see the note on quick_race_screen
//     being pushed OVER a main menu, which keeps its own context alive underneath);
//   - calls set_focus_order() once its document has real content, with the `id`s of every
//     clickable/focusable element, IN NAVIGATION ORDER, and - whenever the first of them is
//     not the screen's MAIN action - the id that should start focused;
//   - overrides on_activate(id) for what Enter/click/gamepad-A on each id does - this is the
//     ONLY place that logic lives, so the three input methods can never drift apart;
//   - overrides on_back() only if Escape/gamepad-B must NOT simply pop (the profile's first run,
//     which cannot be skipped).
//
// Focus is RmlUi's own ':focus'/':focus-visible' pseudo-classes (Element::Focus()) - a
// subclass's RCSS styles focus exactly like it already styles :hover, and the two stay visually
// consistent for free.
#pragma once

#include <string>
#include <vector>

#include "app/screen.h"

namespace Rml {
class Context;
class ElementDocument;
class Element;
}
union SDL_Event;

namespace bigred {

class RmlUiScreen : public Screen {
public:
    // ⚠️ EVERY SCREEN IS LAID OUT IN `dp`, NEVER IN `px`, against a 960x720 canvas - the one the
    // default 1280x720 window gives. A `dp` is canvas_h / 720 real pixels, so a layout that fits
    // at 720 lines fits at every size: the same picture, scaled. Written in `px` (before
    // 2026-09-26) the text stayed the same size whatever the canvas, and a screen designed on
    // one window overflowed on a smaller one - the stacked, overlapping rows the user reported.
    // The font still rounds each size DOWN to a whole multiple of its 8-pixel artwork
    // (ui/rml_font_engine.h), so text is never larger than the box laid out for it.
    // `openbigred --shots` checks every screen at several sizes (game/menu_shots.h).
    static constexpr float kReferenceCanvasHeight = 720.0f;
    static float dp_ratio(int canvas_h);

    bool enter(App& app) override;
    void exit(App& app) override;
    bool handle_event(App& app, const SDL_Event& e) override;
    void update(App& app, double dt_ms) override;
    void draw(App& app) override;
    bool uses_canvas() const override { return true; }

protected:
    virtual const char* document_source() const = 0;
    virtual const char* context_name() const = 0;

    // The document exists and is Show()'n; build dynamic content and call set_focus_order().
    virtual void on_ready(App& app) { (void)app; }

    virtual void on_activate(App& app, const std::string& id) { (void)app; (void)id; }
    // Out of line: the default body calls App::pop(), and App is only forward-declared here
    // (screen.h's own pattern) - defined in rml_screen.cpp, where app/app.h is included.
    virtual void on_back(App& app);

    // ⚠️ `initial` IS WHAT THE BOTTOM PAD BUTTON DOES ON ARRIVAL, and it is worth naming.
    // The focus starts on the first id when nothing is said, which is right for a list of
    // choices and wrong for a screen whose first widget is an arrow: on the globe, a player
    // who pressed the bottom button got the LEFT ARROW - the screen turned instead of
    // validating, and it read as "the button does nothing". Name the main action instead.
    void set_focus_order(std::vector<std::string> ids, const std::string& initial = {});
    // Re-reads element positions after a rebuild changed WHICH element an id points to (a list
    // rebuilt with SetInnerRML makes new Element objects). Cheap: only walks focus_order_.
    void refresh_focus_elements();
    // The id focused now, or empty. A screen that rebuilds itself while the player is on it
    // passes this back to set_focus_order() so the rebuild does not throw the focus away.
    std::string focused_id() const {
        return (focus_index_ >= 0 && static_cast<size_t>(focus_index_) < focus_order_.size())
                   ? focus_order_[static_cast<size_t>(focus_index_)]
                   : std::string();
    }

    Rml::Context* context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    int canvas_w_ = 0, canvas_h_ = 0;

private:
    void move_focus(App& app, int delta);
    void activate_focused(App& app);
    void sync_focus_from_hover(Rml::Element* hover);

    std::vector<std::string> focus_order_;
    std::vector<Rml::Element*> focus_elements_;
    int focus_index_ = -1;
    // Edge-triggering for the analogue stick - see handle_event()'s SDL_CONTROLLERAXISMOTION
    // case. Per-screen, not a function-local static: a static would leak "armed" state from
    // whichever screen last touched the stick into the next one entered.
    bool axis_armed_neg_ = true, axis_armed_pos_ = true;
};

} // namespace bigred
