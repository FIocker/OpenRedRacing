#include "gen/hill_paint.h"

#include <algorithm>
#include <cstdlib>

namespace bigred::gen {

namespace {

constexpr int kMap = 256;
constexpr int kV = kMap + 1;   // vertices per side
constexpr int kRaised = 2;     // bytes above the plain: less is flat

size_t vk(int x, int z) { return static_cast<size_t>(z) * kV + x; }

// A vertex's height: the texel it is the corner (x0, z0) of, the last row and column repeated.
int vertex_height(const Blob& mph, int x, int z) {
    return mph[static_cast<size_t>(std::min(z, kMap - 1)) * kMap + std::min(x, kMap - 1)];
}

// Each vertex's height above the plain, summed over its 3x3 vertices: the rank that decides its
// ground (a sum, so a knob's top outranks its shoulder at the same height).
std::vector<int> rank_scores(const Blob& mph, int base) {
    std::vector<int> ex(static_cast<size_t>(kV) * kV), s(ex.size(), 0);
    for (int z = 0; z < kV; ++z)
        for (int x = 0; x < kV; ++x) ex[vk(x, z)] = vertex_height(mph, x, z) - base;
    for (int z = 0; z < kV; ++z)
        for (int x = 0; x < kV; ++x) {
            int sum = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) sum += ex[vk(std::clamp(x + dx, 0, kV - 1), std::clamp(z + dz, 0, kV - 1))];
            s[vk(x, z)] = sum;
        }
    return s;
}

} // namespace

HillPaint learn_hill_paint(const AutoTiles& at, const Blob& mph, const Blob& mpc, uint8_t road_mpc, int base_height) {
    HillPaint hp;
    hp.base_height = base_height;
    if (!at.valid() || mph.size() != static_cast<size_t>(kMap) * kMap || mpc.size() != mph.size()) return hp;
    const int M = at.materials;
    const std::vector<int8_t> V = vertex_materials(at, mpc);
    const std::vector<int> score = rank_scores(mph, base_height);
    // The steps under each vertex, to leave out the noise off a playable area. NOT the grounds
    // of slopes: DIRT2's medium grass lives on the flanks and is as steep as a cliff; the real
    // cliffs (DIRT3's terraces, WATER's rocks) are under the 3 % a hill ground needs.
    std::vector<int> step(V.size(), 0);
    for (int z = 0; z < kV; ++z)
        for (int x = 0; x < kV; ++x) {
            const int hv = vertex_height(mph, x, z);
            int s = 0;
            if (x + 1 < kV) s = std::max(s, std::abs(hv - vertex_height(mph, x + 1, z)));
            if (x > 0) s = std::max(s, std::abs(hv - vertex_height(mph, x - 1, z)));
            if (z + 1 < kV) s = std::max(s, std::abs(hv - vertex_height(mph, x, z + 1)));
            if (z > 0) s = std::max(s, std::abs(hv - vertex_height(mph, x, z - 1)));
            step[vk(x, z)] = s;
        }
    std::array<bool, AutoTiles::kMaxMaterials> left_out{};
    for (int m = 0; m < M; ++m) left_out[static_cast<size_t>(m)] = at.wet[static_cast<size_t>(m)];
    const int road = at.plain_of[road_mpc];
    if (road >= 0) left_out[static_cast<size_t>(road)] = true;
    // The plain's ground: the most common on the flat land. The hill grounds: 3 % or more of the
    // raised land, ranked by the median score of their vertices.
    std::array<int, AutoTiles::kMaxMaterials> flat{}, raised{};
    std::array<std::vector<int>, AutoTiles::kMaxMaterials> scores;
    int raised_total = 0;
    for (int z = 0; z < kV; ++z)
        for (int x = 0; x < kV; ++x) {
            const size_t v = vk(x, z);
            const int m = V[v];
            if (m < 0 || left_out[static_cast<size_t>(m)] || step[v] > 60) continue;
            if (vertex_height(mph, x, z) - base_height <= kRaised) {
                ++flat[static_cast<size_t>(m)];
            } else {
                ++raised[static_cast<size_t>(m)];
                scores[static_cast<size_t>(m)].push_back(score[v]);
                ++raised_total;
            }
        }
    for (int m = 0; m < M; ++m)
        if (flat[static_cast<size_t>(m)] > 0 && (hp.base < 0 || flat[static_cast<size_t>(m)] > flat[static_cast<size_t>(hp.base)])) hp.base = m;
    std::vector<std::pair<int, int>> ranked;   // (median score, ground)
    for (int m = 0; m < M; ++m) {
        if (raised_total == 0 || raised[static_cast<size_t>(m)] * 1000 < raised_total * 30) continue;
        std::vector<int>& s = scores[static_cast<size_t>(m)];
        std::nth_element(s.begin(), s.begin() + static_cast<long>(s.size() / 2), s.end());
        ranked.push_back({s[s.size() / 2], m});
    }
    std::sort(ranked.begin(), ranked.end());
    int kept = 0;
    for (const auto& r : ranked) kept += raised[static_cast<size_t>(r.second)];
    int given = 0;
    for (size_t i = 0; i < ranked.size(); ++i) {
        const int m = ranked[i].second;
        const int sh = i + 1 == ranked.size() ? 1000 - given : raised[static_cast<size_t>(m)] * 1000 / std::max(kept, 1);
        hp.order.push_back(m);
        hp.share.push_back(sh);
        given += sh;
    }
    if (hp.base >= 0) hp.paintable[static_cast<size_t>(hp.base)] = true;
    for (int m : hp.order) hp.paintable[static_cast<size_t>(m)] = true;
    hp.report = "peinture des collines : plaine " + std::to_string(hp.base) + ", collines";
    for (size_t i = 0; i < hp.order.size(); ++i)
        hp.report += " " + std::to_string(hp.order[i]) + " (" + std::to_string(hp.share[i] / 10) + " %)";
    return hp;
}

int paint_hills(const HillPaint& hp, std::vector<int8_t>& V, const Blob& mph, const std::vector<uint8_t>& keep,
                std::vector<uint8_t>* top) {
    if (!hp.valid() || V.size() != static_cast<size_t>(kV) * kV || mph.size() != static_cast<size_t>(kMap) * kMap) return 0;
    const std::vector<int> score = rank_scores(mph, hp.base_height);
    std::vector<std::pair<int, uint32_t>> raised;   // (score, vertex): the order of the ranks
    int changed = 0;
    for (int z = 0; z < kV; ++z)
        for (int x = 0; x < kV; ++x) {
            const size_t v = vk(x, z);
            if (V[v] < 0 || !hp.paintable[static_cast<size_t>(V[v])] || (!keep.empty() && keep[v])) continue;
            if (vertex_height(mph, x, z) - hp.base_height <= kRaised) {
                changed += V[v] != hp.base;
                V[v] = static_cast<int8_t>(hp.base);
            } else {
                raised.push_back({score[v], static_cast<uint32_t>(v)});
            }
        }
    std::sort(raised.begin(), raised.end());
    if (top) top->assign(V.size(), 0);
    size_t from = 0;
    int acc = 0;
    for (size_t i = 0; i < hp.order.size(); ++i) {
        acc += hp.share[i];
        const size_t to = i + 1 == hp.order.size() ? raised.size() : raised.size() * static_cast<size_t>(acc) / 1000;
        for (size_t j = from; j < to; ++j) {
            const size_t v = raised[j].second;
            changed += V[v] != hp.order[i];
            V[v] = static_cast<int8_t>(hp.order[i]);
            if (top && i + 1 == hp.order.size()) (*top)[v] = 1;
        }
        from = to;
    }
    return changed;
}

} // namespace bigred::gen
