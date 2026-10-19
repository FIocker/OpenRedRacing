#include "rml_system_interface.h"

#include <SDL.h>

#include <cstdio>

namespace bigred {

double RmlSystemInterface::GetElapsedTime() {
    return static_cast<double>(SDL_GetTicks64()) / 1000.0;
}

bool RmlSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    const char* level = "info";
    switch (type) {
    case Rml::Log::LT_ERROR: level = "error"; break;
    case Rml::Log::LT_ASSERT: level = "assert"; break;
    case Rml::Log::LT_WARNING: level = "warning"; break;
    default: break;
    }
    // Everything below a warning is dropped: RmlUi logs an info line per document and per
    // texture, and a menu that loads cleanly should say nothing at all.
    if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING)
        std::fprintf(stderr, "RmlUi %s: %s\n", level, message.c_str());
    return true;
}

} // namespace bigred
