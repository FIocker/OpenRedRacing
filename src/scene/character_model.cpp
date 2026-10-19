#include "character_model.h"

#include <cstdio>
#include <stdexcept>

#include "formats/game_files.h"

namespace bigred {
namespace {

// The original's character table, case by case. The body tables are the ones the original fills
// from the strings "BLOKE", "BLOKEB", "WOMAN" and "WOMANB"; the two coloured heads from "HEAD1"
// and "HEAD4".
//
// The skin agrees, which is how this was first read before the code confirmed it: BLOKEB, WOMANB and
// heads 3, 4x and 6 paint their skin with palette 10..15, the others with 240..245 - every
// driver's head matches its body.
constexpr CharacterInfo kCharacters[kCharacterCount] = {
    {"DEREK", "CHAR1", "BLOKE", "HEAD1", true, 14, -38},    // "colour coordinated Mohawk"
    {"NICOLE", "CHAR2", "WOMAN", "HEAD2", false, 19, -43},  // "piggy tails"
    {"JAKE", "CHAR3", "BLOKEB", "HEAD3", false, 14, -38},   // "flat top"
    {"GEORGIO", "CHAR4", "BLOKEB", "HEAD4", true, 14, -38},
    {"IVAN", "CHAR5", "BLOKE", "HEAD5", false, 14, -38},    // "goatee beard"
    {"HELGA", "CHAR6", "WOMANB", "HEAD6", false, 19, -43},  // "spider head"
};

const Blob* find(const std::unordered_map<std::string, Blob>& a, const std::string& name) {
    auto it = a.find(name);
    return it == a.end() ? nullptr : &it->second;
}

} // namespace

const CharacterInfo& character_info(int index) {
    return kCharacters[((index % kCharacterCount) + kCharacterCount) % kCharacterCount];
}

bool load_character_shapes(const std::string& game_dir, int index, CharacterShapes& out) {
    out = CharacterShapes{};
    const Archive& scn = *game_archive_or_empty(game_dir, "CHARSCN.SCN");
    if (scn.empty()) return false;

    const CharacterInfo& info = character_info(index);
    const Blob* body = find(scn, std::string(info.body) + "1.3DW");
    const Blob* head = find(scn, std::string(info.head) + (info.head_coloured ? "1" : "") + ".3DW");
    if (!body || !head) return false;
    try {
        out.body = parse_3dw(body->data(), body->size());
        out.head = parse_3dw(head->data(), head->size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "character %d: %s\n", index, e.what());
        return false;
    }
    if (const Blob* mouth = find(scn, "MOUTH.TM")) out.mouth_tm = *mouth;
    // The original's heights grow DOWNWARDS (+Y down, like every .3DW); the render space grows
    // up. Its units are the true ones, which the halved render space already is.
    out.head_rise = static_cast<float>(info.body_height - info.head_height);
    out.valid = true;
    return true;
}

} // namespace bigred
