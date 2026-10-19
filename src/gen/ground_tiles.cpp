// The ground's tiles, learned and laid again. See the header.

#include "gen/ground_tiles.h"

#include "formats/terrain.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <tuple>

namespace bigred::gen {

namespace {

constexpr int kMap = 256;
constexpr int kTile = 32;
constexpr int kPatch = 6;            // the corner patch a material is read from, pixels
constexpr int kSameMaterial = 28;    // RGB distance under which two corner colours are one ground

int dist2(const std::array<int, 3>& a, const std::array<int, 3>& b) {
    const int r = a[0] - b[0], g = a[1] - b[1], bl = a[2] - b[2];
    return r * r + g * g + bl * bl;
}

// The biggest step, in height bytes, from each cell to one of its four neighbours.
std::vector<int> cell_steps(const Blob& mph, int n) {
    std::vector<int> s(static_cast<size_t>(n) * n, 0);
    for (int gz = 0; gz < n; ++gz)
        for (int gx = 0; gx < n; ++gx) {
            const size_t k = static_cast<size_t>(gz) * n + gx;
            int best = 0;
            if (gx + 1 < n) best = std::max(best, std::abs(static_cast<int>(mph[k]) - mph[k + 1]));
            if (gx > 0) best = std::max(best, std::abs(static_cast<int>(mph[k]) - mph[k - 1]));
            if (gz + 1 < n) best = std::max(best, std::abs(static_cast<int>(mph[k]) - mph[k + n]));
            if (gz > 0) best = std::max(best, std::abs(static_cast<int>(mph[k]) - mph[k - n]));
            s[k] = best;
        }
    return s;
}

} // namespace

GroundTiles learn_ground_tiles(const Archive& donor, const std::string& level_name, int outside_code) {
    GroundTiles gt;
    for (auto& c : gt.corners) c = {-1, -1, -1, -1};
    const Blob* mpc = find_member(donor, "MAP.MPC");
    const Blob* mph = find_member(donor, "MAP.MPH");
    const Blob* cht = find_member(donor, "MAP.CHT");
    const Blob* col = find_member(donor, "MAP.COL");
    const Blob* txt = find_member(donor, level_name + ".TXT");
    if (!mpc || !mph || !cht || !col || !txt || mpc->size() != static_cast<size_t>(kMap) * kMap ||
        cht->size() < 768 || col->size() < 768)
        return gt;
    const std::optional<TexesDirective> texes = parse_texes_directive(std::string(txt->begin(), txt->end()));
    if (!texes) return gt;
    std::vector<const Blob*> atlases;
    for (const std::string& nm : texes_atlas_filenames(*texes)) {
        const Blob* a = find_member(donor, nm + ".TM");
        if (!a || a->size() < 256 * 192) break;
        atlases.push_back(a);
    }
    if (atlases.empty()) return gt;
    for (uint8_t c : *mpc) ++gt.count[c];
    {
        std::array<std::array<int, 4>, 256> par{};
        for (int gz = 0; gz < kMap; ++gz)
            for (int gx = 0; gx < kMap; ++gx) ++par[(*mpc)[static_cast<size_t>(gz) * kMap + gx]][(gx & 1) + 2 * (gz & 1)];
        for (int code = 0; code < 256; ++code) {
            gt.parity[static_cast<size_t>(code)] = -1;
            for (int p = 0; p < 4; ++p)
                if (gt.count[static_cast<size_t>(code)] >= 40 && par[code][p] * 10 >= gt.count[static_cast<size_t>(code)] * 9)
                    gt.parity[static_cast<size_t>(code)] = static_cast<int8_t>(p);
        }
    }

    // Every used code's four corner colours: the mean of a 6x6 patch in each corner of its tile,
    // through the level's palette (6-bit VGA, x4). The atlas is stored V-flipped
    // (formats/terrain.md): the tile's z0 edge is the LAST row of its block.
    std::array<std::array<std::array<int, 3>, 4>, 256> rgb{};
    std::array<bool, 256> has_tile{};
    for (int code = 0; code < 256; ++code) {
        if (gt.count[static_cast<size_t>(code)] == 0) continue;
        const int t = (*cht)[512 + static_cast<size_t>(code)];
        if (t >= 192 || t / 48 >= static_cast<int>(atlases.size())) continue;
        const Blob& a = *atlases[static_cast<size_t>(t / 48)];
        const int st = t % 48, tx = (st & 7) * kTile, ty = (st >> 3) * kTile;
        for (int corner = 0; corner < 4; ++corner) {
            const int cx = corner & 1, cz = corner >> 1;
            int sum[3] = {0, 0, 0};
            for (int py = 0; py < kPatch; ++py)
                for (int px = 0; px < kPatch; ++px) {
                    const int u = cx ? kTile - 1 - px : px;       // tile pixel, x
                    const int v = cz ? kTile - 1 - py : py;       // tile pixel, z
                    const uint8_t idx = a[static_cast<size_t>(ty + kTile - 1 - v) * 256 + tx + u];
                    for (int ch = 0; ch < 3; ++ch) sum[ch] += std::min(255, (*col)[idx * 3u + ch] * 4);
                }
            for (int ch = 0; ch < 3; ++ch) rgb[code][corner][ch] = sum[ch] / (kPatch * kPatch);
        }
        has_tile[static_cast<size_t>(code)] = true;
    }

    // The materials: corner colours gathered greedily, the most used first.
    std::vector<std::tuple<int, int, int>> order;   // (-count, code, corner)
    for (int code = 0; code < 256; ++code)
        if (has_tile[static_cast<size_t>(code)])
            for (int corner = 0; corner < 4; ++corner) order.emplace_back(-gt.count[static_cast<size_t>(code)], code, corner);
    std::sort(order.begin(), order.end());
    std::vector<std::array<int, 3>> centre;
    for (const auto& [neg, code, corner] : order) {
        const std::array<int, 3>& c = rgb[code][corner];
        bool near = false;
        for (const auto& m : centre) near = near || dist2(c, m) < kSameMaterial * kSameMaterial;
        if (!near && centre.size() < 64) centre.push_back(c);
    }
    auto nearest = [&](const std::array<int, 3>& c) {
        int best = 0;
        for (size_t m = 1; m < centre.size(); ++m)
            if (dist2(c, centre[m]) < dist2(c, centre[static_cast<size_t>(best)])) best = static_cast<int>(m);
        return best;
    };
    std::array<std::array<int, 4>, 256> raw{};
    for (int code = 0; code < 256; ++code)
        if (has_tile[static_cast<size_t>(code)])
            for (int corner = 0; corner < 4; ++corner) raw[code][corner] = nearest(rgb[code][corner]);
    // A material with no PURE tile (four equal corners) is a blend of corner colours, not a
    // ground: it folds into the nearest one that has. The rest are numbered in order.
    std::vector<int> pure_id(centre.size(), -1);
    std::vector<int> pure_list;
    for (int code = 0; code < 256; ++code) {
        if (!has_tile[static_cast<size_t>(code)]) continue;
        const auto& r = raw[code];
        if (r[0] == r[1] && r[1] == r[2] && r[2] == r[3] && pure_id[static_cast<size_t>(r[0])] < 0 &&
            static_cast<int>(pure_list.size()) < GroundTiles::kMaxMaterials) {
            pure_id[static_cast<size_t>(r[0])] = static_cast<int>(pure_list.size());
            pure_list.push_back(r[0]);
        }
    }
    if (pure_list.empty()) return gt;
    std::sort(pure_list.begin(), pure_list.end());
    for (size_t i = 0; i < pure_list.size(); ++i) pure_id[static_cast<size_t>(pure_list[i])] = static_cast<int>(i);
    std::vector<int> fold(centre.size());
    for (size_t m = 0; m < centre.size(); ++m) {
        if (pure_id[m] >= 0) { fold[m] = pure_id[m]; continue; }
        int best = pure_list[0];
        for (int p : pure_list)
            if (dist2(centre[m], centre[static_cast<size_t>(p)]) < dist2(centre[m], centre[static_cast<size_t>(best)])) best = p;
        fold[m] = pure_id[static_cast<size_t>(best)];
    }
    gt.materials = static_cast<int>(pure_list.size());
    for (size_t i = 0; i < pure_list.size(); ++i) gt.colour[i] = centre[static_cast<size_t>(pure_list[i])];
    for (int code = 0; code < 256; ++code) {
        if (!has_tile[static_cast<size_t>(code)]) continue;
        for (int corner = 0; corner < 4; ++corner) {
            const int m = fold[static_cast<size_t>(raw[code][corner])];
            gt.corners[static_cast<size_t>(code)][corner] = static_cast<int8_t>(m);
            gt.weight[static_cast<size_t>(m)] += gt.count[static_cast<size_t>(code)];
        }
    }
    // Two grounds may touch when the donor has a real TRANSITION between them: a tile of exactly
    // those two, used at least 10 times. Any tile holding both is not enough - WATER2's code 97
    // has sand, water and cliff at once, and water then met cliff where no edge tile exists.
    {
        std::array<std::array<int, GroundTiles::kMaxMaterials>, GroundTiles::kMaxMaterials> pair_cells{};
        for (int code = 0; code < 256; ++code) {
            const auto& c = gt.corners[static_cast<size_t>(code)];
            if (c[0] < 0) continue;
            int a = c[0], b = c[0];
            bool two = true;
            for (int8_t m : c) {
                if (m == a || m == b) continue;
                if (a == b) b = m;
                else two = false;
            }
            if (two && a != b) {
                pair_cells[static_cast<size_t>(a)][static_cast<size_t>(b)] += gt.count[static_cast<size_t>(code)];
                pair_cells[static_cast<size_t>(b)][static_cast<size_t>(a)] += gt.count[static_cast<size_t>(code)];
            }
        }
        for (int a = 0; a < gt.materials; ++a)
            for (int b = 0; b < gt.materials; ++b)
                gt.touch[static_cast<size_t>(a)][static_cast<size_t>(b)] = a == b || pair_cells[static_cast<size_t>(a)][static_cast<size_t>(b)] >= 10;
    }

    // THE SEA: a material that is FLAT (three quarters of its pure cells without a step), NO
    // HIGHER THAN THE LAND (the median of every cell) and blue - the outside round an island
    // (DIRT, SNOW) and CITY2's harbour alike. Not "the lowest level": DIRT3 has hollows under its
    // sea. CHOPPER's outside is green grass: not a sea, and never flattened as one. The outside's
    // own material is tried first, then the most used that qualifies.
    {
        const std::vector<int> st = cell_steps(*mph, kMap);
        std::vector<int> all_h(mph->begin(), mph->end());
        std::sort(all_h.begin(), all_h.end());
        const int land_median_h = all_h[all_h.size() / 2];
        auto is_sea = [&](int m) {
            const auto& c = gt.colour[static_cast<size_t>(m)];
            if (c[2] < c[0] + 24 || c[2] < c[1] + 16) return false;
            std::vector<int> s, h;
            for (size_t k = 0; k < mpc->size(); ++k) {
                const auto& q = gt.corners[(*mpc)[k]];
                if (q[0] == m && q[1] == m && q[2] == m && q[3] == m) {
                    s.push_back(st[k]);
                    h.push_back((*mph)[k]);
                }
            }
            if (s.size() < 200) return false;
            std::sort(s.begin(), s.end());
            std::sort(h.begin(), h.end());
            return s[s.size() * 3 / 4] <= 1 && h[h.size() / 2] <= land_median_h;
        };
        if (outside_code >= 0) {
            const auto& c = gt.corners[static_cast<size_t>(outside_code)];
            if (c[0] >= 0 && c[0] == c[1] && c[1] == c[2] && c[2] == c[3] && is_sea(c[0])) gt.sea = c[0];
        }
        for (int m = 0; m < gt.materials && gt.sea < 0; ++m)
            if (is_sea(m)) gt.sea = m;
        if (gt.sea >= 0) {
            std::vector<int> h;
            for (size_t k = 0; k < mpc->size(); ++k) {
                const auto& q = gt.corners[(*mpc)[k]];
                if (q[0] == gt.sea && q[1] == gt.sea && q[2] == gt.sea && q[3] == gt.sea) h.push_back((*mph)[k]);
            }
            std::sort(h.begin(), h.end());
            gt.sea_level = h[h.size() / 2];
        }
    }
    // Steep materials: the steps under their pure tiles. A cliff is steep AGAINST THE REST OF
    // THE MAP and a minority: a median step of 12 bytes or more (192 units a texel, 37 %), at
    // least twice the median of all the land, on under a quarter of it. SNOW2's snow and MOON2's
    // dust are steep too - those circuits are mountains - but they are the ground itself.
    const std::vector<int> steps = cell_steps(*mph, kMap);
    std::vector<int> land_steps;
    int64_t land_weight = 0;
    for (size_t k = 0; k < mpc->size(); ++k) {
        const auto& c = gt.corners[(*mpc)[k]];
        if (c[0] >= 0 && c[0] != gt.sea) land_steps.push_back(steps[k]);
    }
    for (int m = 0; m < gt.materials; ++m)
        if (m != gt.sea) land_weight += gt.weight[static_cast<size_t>(m)];
    std::sort(land_steps.begin(), land_steps.end());
    const int land_median = land_steps.empty() ? 0 : land_steps[land_steps.size() / 2];
    for (int m = 0; m < gt.materials; ++m) {
        if (m == gt.sea || gt.weight[static_cast<size_t>(m)] * 4 >= land_weight) continue;
        std::vector<int> s;
        for (size_t k = 0; k < mpc->size(); ++k) {
            const auto& c = gt.corners[(*mpc)[k]];
            if (c[0] == m && c[1] == m && c[2] == m && c[3] == m) s.push_back(steps[k]);
        }
        if (s.size() < 20) continue;
        std::sort(s.begin(), s.end());
        const int median = s[s.size() / 2];
        if (median >= 12 && median >= 2 * std::max(1, land_median))
            gt.cliff_step[static_cast<size_t>(m)] = std::max(1, s[s.size() / 4]);
    }
    char buf[96];
    gt.report = "tuiles de " + level_name + " :";
    for (int m = 0; m < gt.materials; ++m) {
        std::snprintf(buf, sizeof buf, " %d(%d,%d,%d)%s%s", m, gt.colour[static_cast<size_t>(m)][0],
                      gt.colour[static_cast<size_t>(m)][1], gt.colour[static_cast<size_t>(m)][2],
                      m == gt.sea ? "=mer" : "", gt.cliff_step[static_cast<size_t>(m)] ? "=falaise" : "");
        gt.report += buf;
    }
    return gt;
}

std::string lay_ground_tiles(const GroundTiles& gt, const Blob& mph, Blob& mpc,
                             const std::vector<uint8_t>& road, const std::vector<uint8_t>& water,
                             const std::array<bool, 256>& road_code) {
    if (!gt.valid()) return "tuiles : aucune (donneur illisible)";
    constexpr int kV = kMap + 1;   // vertices per side
    const int M = gt.materials;
    auto vk = [](int vx, int vz) { return static_cast<size_t>(vz) * kV + vx; };
    auto ck = [](int gx, int gz) { return static_cast<size_t>(gz) * kMap + gx; };
    // The cells round a vertex, and which of their corners it is.
    struct Around { int gx, gz, corner; };
    auto around = [](int vx, int vz, Around out[4]) {
        out[0] = {vx - 1, vz - 1, 3};
        out[1] = {vx, vz - 1, 2};
        out[2] = {vx - 1, vz, 1};
        out[3] = {vx, vz, 0};
    };
    auto inside = [](int gx, int gz) { return gx >= 0 && gz >= 0 && gx < kMap && gz < kMap; };
    const std::vector<int> steps = cell_steps(mph, kMap);

    // 1. The votes of the copied tiles. A tie goes to the ground that is not the sea (the shore
    // tiles are the sea's cells; its neighbours stay land), then to the more common ground.
    std::vector<int8_t> V(static_cast<size_t>(kV) * kV, -1);
    std::vector<uint8_t> slope(V.size(), 0), near_road(V.size(), 0), all_water(V.size(), 0);
    for (int vz = 0; vz < kV; ++vz)
        for (int vx = 0; vx < kV; ++vx) {
            int votes[GroundTiles::kMaxMaterials] = {};
            Around a[4];
            around(vx, vz, a);
            int cells = 0, wet = 0, sl = 0;
            bool by_road = false;
            for (const Around& c : a) {
                if (!inside(c.gx, c.gz)) continue;
                const size_t k = ck(c.gx, c.gz);
                ++cells;
                wet += water[k] ? 1 : 0;
                by_road = by_road || road[k];
                sl = std::max(sl, steps[k]);
                const int m = gt.corners[mpc[k]][c.corner];
                if (m >= 0) ++votes[m];
            }
            int best = -1;
            for (int m = 0; m < M; ++m) {
                if (votes[m] == 0) continue;
                if (best < 0 || votes[m] > votes[best] ||
                    (votes[m] == votes[best] && (best == gt.sea || (m != gt.sea && gt.weight[static_cast<size_t>(m)] > gt.weight[static_cast<size_t>(best)]))))
                    best = m;
            }
            const size_t v = vk(vx, vz);
            V[v] = static_cast<int8_t>(best);
            slope[v] = static_cast<uint8_t>(std::min(sl, 255));
            near_road[v] = by_road ? 1 : 0;
            all_water[v] = cells > 0 && wet == cells ? 1 : 0;
        }
    // The most common level, dry ground: what a misplaced cliff or a lost vertex becomes.
    int base = -1;
    int steepest_cliff = -1;
    for (int m = 0; m < M; ++m) {
        if (m == gt.sea) continue;
        if (!gt.cliff_step[static_cast<size_t>(m)] && (base < 0 || gt.weight[static_cast<size_t>(m)] > gt.weight[static_cast<size_t>(base)])) base = m;
        if (gt.cliff_step[static_cast<size_t>(m)] && (steepest_cliff < 0 || gt.weight[static_cast<size_t>(m)] > gt.weight[static_cast<size_t>(steepest_cliff)]))
            steepest_cliff = m;
    }
    if (base < 0) base = 0;
    auto fixed = [&](size_t v) { return all_water[v] != 0; };
    for (size_t v = 0; v < V.size(); ++v) {
        if (V[v] < 0) V[v] = static_cast<int8_t>(base);
        if (all_water[v] && gt.sea >= 0) V[v] = static_cast<int8_t>(gt.sea);
    }
    // The ground most common round a vertex, cliffs and the sea left out.
    auto level_ground_round = [&](int vx, int vz) {
        int n[GroundTiles::kMaxMaterials] = {};
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const int x = vx + dx, z = vz + dz;
                if ((dx || dz) && x >= 0 && z >= 0 && x < kV && z < kV) {
                    const int m = V[vk(x, z)];
                    if (m != gt.sea && !gt.cliff_step[static_cast<size_t>(m)]) ++n[m];
                }
            }
        int best = base;
        for (int m = 0; m < M; ++m)
            if (n[m] > n[best]) best = m;
        return best;
    };

    // 2. Cliffs where it climbs, and only there. Never by the road nor in the water.
    int cliffs_removed = 0, cliffs_added = 0;
    for (int vz = 0; vz < kV; ++vz)
        for (int vx = 0; vx < kV; ++vx) {
            const size_t v = vk(vx, vz);
            if (fixed(v)) continue;
            const int m = V[v];
            const int cs = gt.cliff_step[static_cast<size_t>(m)];
            if (cs && (slope[v] < cs || near_road[v])) {
                V[v] = static_cast<int8_t>(level_ground_round(vx, vz));
                ++cliffs_removed;
            } else if (!cs && steepest_cliff >= 0 && !near_road[v] && m != gt.sea &&
                       slope[v] >= 2 * gt.cliff_step[static_cast<size_t>(steepest_cliff)]) {
                V[v] = static_cast<int8_t>(steepest_cliff);
                ++cliffs_added;
            }
        }

    // 3. Specks: a vertex whose neighbours agree on another ground, six out of eight, takes it.
    {
        std::vector<int8_t> next = V;
        for (int vz = 1; vz + 1 < kV; ++vz)
            for (int vx = 1; vx + 1 < kV; ++vx) {
                const size_t v = vk(vx, vz);
                if (fixed(v)) continue;
                int n[GroundTiles::kMaxMaterials] = {};
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (dx || dz) ++n[V[vk(vx + dx, vz + dz)]];
                int best = 0;
                for (int m = 1; m < M; ++m)
                    if (n[m] > n[best]) best = m;
                if (n[best] >= 6 && best != V[v] && best != gt.sea &&
                    !(gt.cliff_step[static_cast<size_t>(best)] && slope[v] < gt.cliff_step[static_cast<size_t>(best)]))
                    next[v] = static_cast<int8_t>(best);
            }
        V = next;
    }

    // 4. Grounds with no tile between them never touch: the vertex takes a ground that has a
    // tile with both (water, then mud, then grass).
    auto bridge = [&](int a, int b) {
        int best = -1;
        for (int m = 0; m < M; ++m)
            if (m != b && gt.touch[static_cast<size_t>(a)][static_cast<size_t>(m)] && gt.touch[static_cast<size_t>(m)][static_cast<size_t>(b)] &&
                (best < 0 || gt.weight[static_cast<size_t>(m)] > gt.weight[static_cast<size_t>(best)]))
                best = m;
        return best;
    };
    int bridged = 0;
    for (int it = 0; it < 6; ++it) {
        int moved = 0;
        for (int vz = 0; vz < kV; ++vz)
            for (int vx = 0; vx < kV; ++vx) {
                const size_t v = vk(vx, vz);
                if (fixed(v)) continue;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int x = vx + dx, z = vz + dz;
                        if (!(dx || dz) || x < 0 || z < 0 || x >= kV || z >= kV) continue;
                        const int a = V[vk(x, z)], b = V[v];
                        if (gt.touch[static_cast<size_t>(a)][static_cast<size_t>(b)]) continue;
                        const int m = bridge(a, b);
                        if (m >= 0 && m != b) {
                            V[v] = static_cast<int8_t>(m);
                            ++moved;
                        }
                    }
            }
        bridged += moved;
        if (!moved) break;
    }

    // 5. Every cell, the tile whose corners are its vertices: the copied one if it already fits,
    // else the donor's most used for those corners (a random one of the pure ones, weighted, for
    // variety), else the nearest by corners. The ROAD'S GROUNDS are the materials of the pure
    // road tiles.
    std::array<bool, GroundTiles::kMaxMaterials> road_ground{};
    for (int code = 0; code < 256; ++code) {
        const auto& c = gt.corners[static_cast<size_t>(code)];
        if (road_code[static_cast<size_t>(code)] && c[0] >= 0 && c[0] == c[1] && c[1] == c[2] && c[2] == c[3])
            road_ground[static_cast<size_t>(c[0])] = true;
    }
    int changed = 0;
    for (int gz = 0; gz < kMap; ++gz)
        for (int gx = 0; gx < kMap; ++gx) {
            const size_t k = ck(gx, gz);
            const int8_t want[4] = {V[vk(gx, gz)], V[vk(gx + 1, gz)], V[vk(gx, gz + 1)], V[vk(gx + 1, gz + 1)]};
            auto allowed = [&](int code) {
                if (gt.count[static_cast<size_t>(code)] == 0 || gt.corners[static_cast<size_t>(code)][0] < 0) return false;
                if (road[k]) return road_code[static_cast<size_t>(code)];
                if (water[k]) return gt.is_water(static_cast<uint8_t>(code));
                return !gt.is_water(static_cast<uint8_t>(code));
            };
            // A motif tile only on its own parity.
            const int par = (gx & 1) + 2 * (gz & 1);
            auto fits = [&](int code) {
                for (int i = 0; i < 4; ++i)
                    if (gt.corners[static_cast<size_t>(code)][i] != want[i]) return false;
                return gt.parity[static_cast<size_t>(code)] < 0 || gt.parity[static_cast<size_t>(code)] == par;
            };
            const int cur = mpc[k];
            int pick = -1;
            if (allowed(cur) && fits(cur)) pick = cur;
            // A road cell whose corners no road tile has (a river's convex bend: one corner of
            // bank) may take any tile that fits and keeps some of the road's ground - rather than
            // the nearest road tile, which is plain water and made the river square.
            if (pick < 0 && road[k]) {
                for (int code = 0; code < 256; ++code) {
                    if (gt.count[static_cast<size_t>(code)] == 0 || gt.corners[static_cast<size_t>(code)][0] < 0 || !fits(code)) continue;
                    bool keeps_road = false;
                    for (int8_t m : gt.corners[static_cast<size_t>(code)]) keeps_road = keeps_road || road_ground[static_cast<size_t>(m)];
                    if (keeps_road && (pick < 0 || gt.count[static_cast<size_t>(code)] > gt.count[static_cast<size_t>(pick)])) pick = code;
                }
            }
            if (pick < 0) {
                const bool pure = want[0] == want[1] && want[1] == want[2] && want[2] == want[3];
                int64_t total = 0;
                for (int code = 0; code < 256; ++code)
                    if (allowed(code) && fits(code)) {
                        total += gt.count[static_cast<size_t>(code)];
                        if (pick < 0 || gt.count[static_cast<size_t>(code)] > gt.count[static_cast<size_t>(pick)]) pick = code;
                    }
                if (pick >= 0 && pure && total > 0) {
                    const uint32_t h = (static_cast<uint32_t>(gx) * 2654435761u) ^ (static_cast<uint32_t>(gz) * 2246822519u);
                    int64_t r = static_cast<int64_t>(h % static_cast<uint32_t>(total));
                    for (int code = 0; code < 256; ++code)
                        if (allowed(code) && fits(code)) {
                            r -= gt.count[static_cast<size_t>(code)];
                            if (r < 0) { pick = code; break; }
                        }
                }
            }
            if (pick < 0) {
                int best_miss = 5;
                for (int code = 0; code < 256; ++code) {
                    if (!allowed(code)) continue;
                    int miss = 0;
                    for (int i = 0; i < 4; ++i) miss += gt.corners[static_cast<size_t>(code)][i] != want[i] ? 1 : 0;
                    if (miss < best_miss || (miss == best_miss && gt.count[static_cast<size_t>(code)] > gt.count[static_cast<size_t>(pick)])) {
                        best_miss = miss;
                        pick = code;
                    }
                }
            }
            if (pick >= 0 && pick != cur) {
                mpc[k] = static_cast<uint8_t>(pick);
                ++changed;
            }
        }
    return "tuiles : " + std::to_string(M) + " matieres, " + std::to_string(changed) + " cases reposees, falaises -" +
           std::to_string(cliffs_removed) + " +" + std::to_string(cliffs_added) + ", " + std::to_string(bridged) + " sommets de transition";
}

} // namespace bigred::gen
