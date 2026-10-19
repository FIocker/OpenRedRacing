#include "rml_screen.h"

#include <RmlUi/Core.h>

#include <SDL.h>

#include <algorithm>
#include <cstdio>

#include "app/app.h"
#include "ui/rml_font_engine.h"

namespace bigred {

void RmlUiScreen::on_back(App& app) { app.pop(); }

float RmlUiScreen::dp_ratio(int canvas_h) {
    return canvas_h > 0 ? static_cast<float>(canvas_h) / kReferenceCanvasHeight : 1.0f;
}

bool RmlUiScreen::enter(App& app) {
    if (!app.ui().init(app.game_dir())) return false;

    canvas_w_ = app.presentation().canvas_width();
    canvas_h_ = app.presentation().canvas_height();
    if (canvas_w_ <= 0 || canvas_h_ <= 0) {
        // The first frame has not sized the canvas yet - the 4:3 box of the current window is
        // the same number the canvas will take.
        const Presentation::Box b = Presentation::box_for(app.width(), app.height());
        canvas_w_ = b.w;
        canvas_h_ = b.h;
    }
    context_ = app.ui().create_context(context_name(), canvas_w_, canvas_h_);
    if (!context_) return false;
    context_->SetDensityIndependentPixelRatio(dp_ratio(canvas_h_));

    // ⚠️ RmlUi's OWN default `display` is `inline` FOR EVERY TAG - there is no browser-like UA
    // stylesheet giving `<div>` a block default the way HTML does. A screen that forgets to
    // state `display: block` on a label/title/row gets it running on the SAME line as its
    // neighbour instead of stacking - the bug the user hit on the options and profile screens
    // (a title flowing into the next line's text, a label sitting beside its value instead of
    // above it). Injected here, once, rather than repeated in every screen's <style>: it goes
    // right after the FIRST <style> tag, before that screen's own rules, so a screen can still
    // override any one element if it ever needs to.
    //
    // The rest of it is what EVERY menu shares, so it is written once (2026-09-26, after the
    // user found the screens crowded and overlapping): the page, its background picture, and
    // the key hint along the bottom - centred, on ONE line, the same place on every screen.
    // A hint longer than about 48 characters does not fit at 16dp; `openbigred --shots`
    // reports it as OUTSIDE.
    std::string doc = document_source();
    static constexpr const char* kBaseStyle =
        "div,p,ul,li,img{display:block;}"
        "body{width:100%;height:100%;font-family:bigred;color:#ffffff;}"
        "#bg{position:absolute;left:0;top:0;width:100%;height:100%;}"
        "#hint{position:absolute;left:2%;width:96%;bottom:2%;font-size:16dp;color:#a0a0c0;"
        "text-align:center;white-space:nowrap;}"
        ".wrap-ok{line-height:1.5;}"
        // THE ORIGINAL'S OWN LAYOUT (ui/scn_kit.h): an element placed where a `.SCN` or a
        // `@X` put it on the 320x200 screen - 3 dp a column, 3.6 dp a line on the 960x720dp
        // canvas - in the wibbly lettering at its own 29-line height.
        // ⚠️ 87dp = 29 rows x 3dp, the COLUMN step, not the line step (3.6dp): the font magnifies
        // by whole numbers, and sized on the lines it grew faster than the columns at 1080 lines
        // (x5 against x4.5) and ran off the right of the screen.
        ".st{position:absolute;font-family:wibs;font-size:87dp;white-space:nowrap;}"
        ".st.b{font-family:wibs-b;}"
        ".spr{position:absolute;}";
    if (const size_t pos = doc.find("<style>"); pos != std::string::npos)
        doc.insert(pos + 7, kBaseStyle);

    document_ = context_->LoadDocumentFromMemory(doc);
    if (!document_) {
        std::fprintf(stderr, "%s: the document did not parse.\n", name());
        return false;
    }
    document_->Show();
    on_ready(app);
    return true;
}

void RmlUiScreen::exit(App& app) {
    // ⚠️ The document goes with the context, and the context must go before the GL objects its
    // geometry holds - see app/app.h's shutdown() ordering. Here only this screen's context.
    document_ = nullptr;
    focus_elements_.clear();
    if (context_) {
        app.ui().remove_context(context_);
        context_ = nullptr;
    }
}

void RmlUiScreen::set_focus_order(std::vector<std::string> ids, const std::string& initial) {
    focus_order_ = std::move(ids);
    focus_index_ = focus_order_.empty() ? -1 : 0;
    if (!initial.empty())
        for (size_t i = 0; i < focus_order_.size(); ++i)
            if (focus_order_[i] == initial) { focus_index_ = static_cast<int>(i); break; }
    refresh_focus_elements();
}

void RmlUiScreen::refresh_focus_elements() {
    focus_elements_.clear();
    if (!document_) return;
    for (const std::string& id : focus_order_)
        focus_elements_.push_back(document_->GetElementById(id));
    if (focus_index_ >= static_cast<int>(focus_elements_.size())) focus_index_ = 0;
}

void RmlUiScreen::move_focus(App&, int delta) {
    if (focus_order_.empty()) return;
    const int n = static_cast<int>(focus_order_.size());
    focus_index_ = ((focus_index_ < 0 ? 0 : focus_index_) + delta + n) % n;
    if (focus_index_ >= 0 && focus_index_ < static_cast<int>(focus_elements_.size())) {
        if (Rml::Element* el = focus_elements_[static_cast<size_t>(focus_index_)])
            el->Focus(/*focus_visible=*/true);
    }
}

void RmlUiScreen::activate_focused(App& app) {
    if (focus_index_ < 0 || focus_index_ >= static_cast<int>(focus_order_.size())) return;
    app.menu_click();
    on_activate(app, focus_order_[static_cast<size_t>(focus_index_)]);
}

// Rule 6 of enchainement-des-menus.md: "un pointeur qui bouge prend le focus" - moving the
// mouse over a focusable element makes it the one Enter/gamepad-A would activate, so switching
// input methods mid-screen never leaves a stale, invisible selection behind.
void RmlUiScreen::sync_focus_from_hover(Rml::Element* hover) {
    if (!hover || focus_order_.empty()) return;
    const Rml::String id = hover->GetId();
    if (id.empty()) return;
    for (size_t i = 0; i < focus_order_.size(); ++i) {
        if (focus_order_[i] == id.c_str()) {
            focus_index_ = static_cast<int>(i);
            hover->Focus(/*focus_visible=*/false);   // mouse-driven: no keyboard focus ring
            return;
        }
    }
}

bool RmlUiScreen::handle_event(App& app, const SDL_Event& e) {
    if (!context_) return false;

    // RmlUi sees everything first, so its own hover/focus state stays right.
    //
    // ⚠️ The pointer is converted to CANVAS coordinates, never window ones: the canvas is a 4:3
    // box inside the window and everything would be off by the letterbox otherwise.
    float cx = 0, cy = 0;
    if (e.type == SDL_MOUSEMOTION) {
        app.mouse_canvas(e.motion.x, e.motion.y, cx, cy);
        sync_focus_from_hover(context_->GetHoverElement());
    } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
        app.mouse_canvas(e.button.x, e.button.y, cx, cy);
    }
    app.ui().handle_event(context_, e, cx, cy);

    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        // ⚠️ WALKS UP TO THE NEAREST ANCESTOR WITH AN ID, not just the exact element under the
        // pointer: a clickable row built out of child <span>s (a label next to a checkbox, say)
        // has its OWN id only on the outer row, so a click landing on an unlabelled child - the
        // checkbox itself, or the label text - found nothing and did nothing. That was the bug
        // the user hit clicking the checkbox added on 2026-09-23.
        //
        // ⚠️ AND IT PREFERS A FOCUSABLE ONE: a value drawn over a whole box with an id of its own,
        // so that refresh() can find it (the options' SPEED readout, a key's name on KEYSCN),
        // swallowed the click - "speed_value" was activated, which nothing answers, instead of
        // "speed" (user, 2026-09-30). The nearest id stays the fallback for screens that give no
        // focus order.
        std::string first_id;
        for (Rml::Element* el = context_->GetHoverElement(); el; el = el->GetParentNode()) {
            const Rml::String id = el->GetId();
            if (id.empty()) continue;
            if (first_id.empty()) first_id = id.c_str();
            if (std::find(focus_order_.begin(), focus_order_.end(), std::string(id.c_str())) != focus_order_.end()) {
                first_id = id.c_str();
                break;
            }
        }
        if (!first_id.empty()) {
            app.menu_click();
            on_activate(app, first_id);
            return true;
        }
    }

    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        switch (e.key.keysym.sym) {
        case SDLK_ESCAPE: on_back(app); return true;
        case SDLK_UP: case SDLK_LEFT: move_focus(app, -1); return true;
        case SDLK_DOWN: case SDLK_RIGHT: move_focus(app, 1); return true;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate_focused(app); return true;
        default: break;
        }
    }

    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            move_focus(app, -1); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            move_focus(app, 1); return true;
        // ⚠️ SDL'S `A` AND `B` ARE POSITIONS, NOT LETTERS: whatever is printed on the pad,
        // SDL_CONTROLLER_BUTTON_A is the BOTTOM face button and _B the RIGHT one. Bottom
        // validates, right goes back - see PadButton in app/app.h. Which element the bottom
        // button hits on arrival is the `initial` of set_focus_order(), not simply the first.
        case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START:
            activate_focused(app); return true;
        case SDL_CONTROLLER_BUTTON_B: on_back(app); return true;
        default: break;
        }
    } else if (e.type == SDL_CONTROLLERAXISMOTION) {
        // Edge-triggered on a threshold crossing, not held: an analogue stick reports every
        // frame it is off-centre, and treating that as repeated "move" would race past every
        // item in a list the instant it is touched.
        static constexpr Sint16 kThreshold = 16000;
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY || e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            const Sint16 v = e.caxis.value;
            if (v < -kThreshold && axis_armed_neg_) { move_focus(app, -1); axis_armed_neg_ = false; return true; }
            if (v > kThreshold && axis_armed_pos_) { move_focus(app, 1); axis_armed_pos_ = false; return true; }
            if (v > -kThreshold / 2) axis_armed_neg_ = true;
            if (v < kThreshold / 2) axis_armed_pos_ = true;
        }
    }

    return false;
}

void RmlUiScreen::update(App& app, double) {
    if (!context_) return;
    // The wibbly lettering runs on the wall clock, whatever the frame rate.
    const uint32_t now = SDL_GetTicks();
    RmlFontEngine::set_clock_ms(now);
    // And so do the original's animated sprites (ui/scn_kit.h, anisprite_rml): every 5 ticks
    // of 14 ms, the next glyph. Only the `src` changes - the element, its hover and its focus
    // stay what they are.
    if (document_) {
        Rml::ElementList anims;
        document_->GetElementsByClassName(anims, "anim");
        const uint32_t step = now / 70;
        for (Rml::Element* e : anims) {
            const int first = e->GetAttribute<int>("data-anim-first", 0);
            const int count = e->GetAttribute<int>("data-anim-count", 1);
            if (count <= 1) continue;
            const Rml::String src = e->GetAttribute<Rml::String>("data-anim-base", "") +
                                    std::to_string(first + static_cast<int>(step % static_cast<uint32_t>(count)));
            if (e->GetAttribute<Rml::String>("src", "") != src) e->SetAttribute("src", src);
        }
    }
    const int w = app.presentation().canvas_width();
    const int h = app.presentation().canvas_height();
    if (w > 0 && h > 0 && (w != canvas_w_ || h != canvas_h_)) {
        canvas_w_ = w;
        canvas_h_ = h;
        context_->SetDimensions(Rml::Vector2i(w, h));
        context_->SetDensityIndependentPixelRatio(dp_ratio(h));
    }
    context_->Update();
}

void RmlUiScreen::draw(App& app) {
    if (!context_) return;
    app.ui().renderer().set_viewport(canvas_w_, canvas_h_);
    app.ui().renderer().begin_frame();
    context_->Render();
    app.ui().renderer().end_frame();
}

} // namespace bigred
