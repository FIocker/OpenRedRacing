// RmlUi's system interface: the clock it measures animations with, and where its log goes.
//
//
// ⚠️ THE CLOCK IS WALL TIME AND THAT IS CORRECT HERE. Menus are display, not simulation: they
// may be driven by a real clock at whatever rate the machine runs. The race's own tick is a
// different clock entirely and nothing here may touch it, and
// nothing periodic in this project counts frames - a player can be at 240 fps.
#pragma once

#include <RmlUi/Core/SystemInterface.h>

namespace bigred {

class RmlSystemInterface : public Rml::SystemInterface {
public:
    double GetElapsedTime() override;
    // RmlUi's warnings are OUR bugs - a missing font, an RML attribute it did not understand -
    // so they go to stderr where the rest of the engine's complaints go, rather than being
    // swallowed. Returning true keeps RmlUi from breaking into a debugger on an assert.
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
};

} // namespace bigred
