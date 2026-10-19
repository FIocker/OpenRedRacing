#include "player_profile.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "formats/bigred_archive.h"

namespace bigred {
namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Trims the ASCII whitespace `std::getline` and the `;` comment strip can leave at either end -
// the pseudo is typed by hand into the file as often as into the create screen.
std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

} // namespace

const std::vector<ColorPreset>& color_presets() {
    // Sober and loud mixed together. Named for the picker, not for any team in the original
    // data - OpenBigRed's colour choice is independent of it.
    static const std::vector<ColorPreset> presets = {
        {"WHITE", 235, 235, 235}, {"SILVER", 180, 180, 188}, {"GRAPHITE", 70, 70, 76},
        {"BLACK", 20, 20, 20},    {"RED", 200, 24, 24},      {"CRIMSON", 140, 8, 40},
        {"ORANGE", 220, 108, 16}, {"AMBER", 224, 168, 16},   {"YELLOW", 232, 208, 24},
        {"OLIVE", 128, 128, 32},  {"LIME", 96, 176, 32},     {"GREEN", 24, 140, 56},
        {"TEAL", 24, 144, 128},   {"CYAN", 40, 176, 200},    {"SKY", 64, 140, 216},
        {"BLUE", 32, 72, 200},    {"NAVY", 24, 32, 120},     {"VIOLET", 112, 56, 200},
        {"PURPLE", 152, 32, 168}, {"MAGENTA", 208, 24, 160}, {"PINK", 232, 120, 168},
        {"BROWN", 120, 76, 40},
    };
    return presets;
}

std::string profile_dir() {
    // SDL_GetPrefPath needs no "org" for a single-developer project; it already ends in a
    // separator. `%APPDATA%\OpenBigRed\` on Windows - not the (read-only, uncommitted) game
    // directory, and not the build output, which a rebuild can wipe.
    char* p = SDL_GetPrefPath("", "OpenBigRed");
    if (!p) return std::string();
    std::string dir(p);
    SDL_free(p);
    return dir;
}

std::array<int, kDriveKeyCount> default_drive_keys() {
    return {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
            SDL_SCANCODE_LCTRL, SDL_SCANCODE_LALT};
}

const char* drive_key_setting(int action) {
    static constexpr const char* kNames[kDriveKeyCount] = {"key_accel", "key_brake", "key_left",
                                                           "key_right", "key_nitro", "key_recover"};
    return action >= 0 && action < kDriveKeyCount ? kNames[action] : "";
}

std::string profile_path() { return profile_dir() + "profile.cfg"; }
std::string logo_dir() { return profile_dir() + "logos\\"; }

bool profile_exists() { return file_exists_utf8(profile_path()); }

PlayerProfile load_profile() {
    PlayerProfile profile;
    const Blob raw = read_file_utf8(profile_path());
    if (raw.empty()) return profile;   // defaults - the caller decides what an absent file means

    std::istringstream in(std::string(raw.begin(), raw.end()));
    std::string line;
    while (std::getline(in, line)) {
        if (const size_t c = line.find(';'); c != std::string::npos) line = line.substr(0, c);
        line = trim(line);
        if (line.empty()) continue;

        const size_t sp = line.find_first_of(" \t");
        const std::string key = upper(sp == std::string::npos ? line : line.substr(0, sp));
        const std::string value = sp == std::string::npos ? std::string() : trim(line.substr(sp + 1));
        if (value.empty() && key != "HIDE_LOGOS") continue;

        if (key == "PSEUDO") profile.pseudo = value;
        else if (key == "CHARACTER") profile.character = std::atoi(value.c_str());
        else if (key == "CHARACTER_COLOR") profile.character_color = std::atoi(value.c_str());
        else if (key == "VEHICLE_COLOR") profile.vehicle_color = std::atoi(value.c_str());
        else if (key == "LOGO") profile.logo = value;
        else if (key == "LOGO_INDEX") profile.logo_index = std::atoi(value.c_str());
        else if (key == "HIDE_LOGOS") profile.hide_other_logos = (value == "1");
        else if (key == "DISPLAY_MODE") profile.display_mode = std::atoi(value.c_str());
        else if (key == "MUSIC_VOLUME") profile.music_volume = std::atoi(value.c_str());
        else if (key == "FX_VOLUME") profile.fx_volume = std::atoi(value.c_str());
        else if (key == "DAMAGE_OPTION") profile.damage_option = std::atoi(value.c_str());
        else if (key == "COMMENTARY") profile.commentary = (value == "1");
        else if (key == "SPLIT_SIDE_BY_SIDE") profile.split_side_by_side = (value == "1");
        else if (key == "SPEED_CLASS") profile.speed_class = std::atoi(value.c_str());
        else {
            // The keys by SDL's own names ("Up", "Left Ctrl"), so the file reads as what it is.
            for (int a = 0; a < kDriveKeyCount; ++a)
                if (key == upper(drive_key_setting(a))) {
                    const SDL_Scancode sc = SDL_GetScancodeFromName(value.c_str());
                    if (sc != SDL_SCANCODE_UNKNOWN) profile.keys[static_cast<size_t>(a)] = sc;
                }
        }
    }

    const int n = static_cast<int>(color_presets().size());
    profile.character_color = ((profile.character_color % n) + n) % n;
    profile.character = ((profile.character % 6) + 6) % 6;   // kCharacterCount
    profile.vehicle_color = ((profile.vehicle_color % n) + n) % n;
    // The logo set's size is only known once LOGOSCN.SCN is opened, so this only rejects a
    // negative from a hand-edited file; the screen wraps again against the real count.
    if (profile.logo_index < 0) profile.logo_index = 0;
    if (profile.display_mode < 0 || profile.display_mode >= kDisplayModeCount)
        profile.display_mode = kDefaultDisplayMode;
    const auto clamp256 = [](int v) { return v < 0 ? 0 : (v > 256 ? 256 : v); };
    profile.music_volume = clamp256(profile.music_volume);
    profile.fx_volume = clamp256(profile.fx_volume);
    profile.damage_option = profile.damage_option < 0 ? 0 : (profile.damage_option > 100 ? 100 : profile.damage_option);
    if (profile.speed_class < 0 || profile.speed_class >= kSpeedClassCount) profile.speed_class = 1;
    return profile;
}

bool save_profile(const PlayerProfile& profile) {
    if (!create_directories_utf8(profile_dir())) return false;
    create_directories_utf8(logo_dir());   // best-effort: an absent logo folder is not fatal

    std::ostringstream out;
    out << "; OpenBigRed player profile - plain text, edit freely.\n"
        << "; Regenerated by the game whenever a screen saves it; your own edits to a\n"
        << "; running game are picked up the next time the profile loads.\n"
        << "pseudo " << profile.pseudo << "\n"
        << "character " << profile.character << "\n"
        << "character_color " << profile.character_color << "\n"
        << "vehicle_color " << profile.vehicle_color << "\n"
        << "logo_index " << profile.logo_index << "\n"
        << "logo " << profile.logo << "\n"
        << "hide_logos " << (profile.hide_other_logos ? "1" : "0") << "\n"
        << "display_mode " << profile.display_mode << "\n"
        << "music_volume " << profile.music_volume << "\n"
        << "fx_volume " << profile.fx_volume << "\n"
        << "damage_option " << profile.damage_option << "\n"
        << "commentary " << (profile.commentary ? "1" : "0") << "\n"
        << "split_side_by_side " << (profile.split_side_by_side ? "1" : "0") << "\n"
        << "speed_class " << profile.speed_class << "\n";
    for (int a = 0; a < kDriveKeyCount; ++a)
        out << drive_key_setting(a) << " "
            << SDL_GetScancodeName(static_cast<SDL_Scancode>(profile.keys[static_cast<size_t>(a)])) << "\n";
    const std::string text = out.str();
    return write_file_utf8(profile_path(), Blob(text.begin(), text.end()));
}

} // namespace bigred
