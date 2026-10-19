#include "gen/track_layers.h"

#include "sim/fixed.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>

namespace bigred::gen {

namespace {

constexpr int kMap = 256;
constexpr int kV = kMap + 1;            // vertices per side
constexpr int32_t kTexel = 512;         // world units per texel (and between two vertices)

int32_t isqrt64(int64_t v) {
    if (v <= 0) return 0;
    int64_t r = 1;
    while (r * r <= v) r <<= 1;
    int64_t lo = r >> 1, hi = r;
    while (lo + 1 < hi) {
        const int64_t mid = (lo + hi) / 2;
        (mid * mid <= v ? lo : hi) = mid;
    }
    return static_cast<int32_t>(lo);
}


size_t vk(int x, int z) { return static_cast<size_t>(z) * kV + x; }

// A vertex's height: the texel it is the corner (x0, z0) of, the last row and column repeated.
int vertex_height(const Blob& mph, int x, int z) {
    return mph[static_cast<size_t>(std::min(z, kMap - 1)) * kMap + std::min(x, kMap - 1)];
}

// The grounds of SLOPES: the median step (height bytes to the steepest of the four neighbours)
// of their vertices is 12 or more - WATER1's rocks 23, WATER2's 25, CHOPPER2's canyon walls 14;
// grass, dirt, sand and water 0 to 7 (measured 2026-10-04).
std::array<bool, AutoTiles::kMaxMaterials> steep_grounds(const AutoTiles& at, const std::vector<int8_t>& V, const Blob& mph) {
    std::array<std::vector<int>, AutoTiles::kMaxMaterials> steps;
    for (int z = 0; z < kV; ++z)
        for (int x = 0; x < kV; ++x) {
            const int m = V[vk(x, z)];
            if (m < 0) continue;
            const int h = vertex_height(mph, x, z);
            int s = 0;
            if (x + 1 < kV) s = std::max(s, std::abs(h - vertex_height(mph, x + 1, z)));
            if (x > 0) s = std::max(s, std::abs(h - vertex_height(mph, x - 1, z)));
            if (z + 1 < kV) s = std::max(s, std::abs(h - vertex_height(mph, x, z + 1)));
            if (z > 0) s = std::max(s, std::abs(h - vertex_height(mph, x, z - 1)));
            steps[static_cast<size_t>(m)].push_back(s);
        }
    std::array<bool, AutoTiles::kMaxMaterials> steep{};
    for (int m = 0; m < at.materials; ++m) {
        std::vector<int>& v = steps[static_cast<size_t>(m)];
        if (v.size() < 20) continue;
        std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
        steep[static_cast<size_t>(m)] = v[v.size() / 2] >= 12;
    }
    return steep;
}

// The donor's lake (see LakeLook): its bodies of water by 4-connectivity, without those that reach
// the border, come within half the track's width plus 1 500 of the road's middle, or lie more
// than 16 000 from it; the largest.
LakeLook learn_lake(const AutoTiles& at, const std::vector<int8_t>& V, const std::vector<Point>& road, int32_t track_width) {
    LakeLook lake;
    const size_t nv = V.size();
    std::vector<int> comp(nv, -1);
    auto wet = [&](size_t k) { return V[k] >= 0 && at.wet[static_cast<size_t>(V[k])]; };
    std::vector<size_t> best;
    int id = 0;
    for (size_t k0 = 0; k0 < nv; ++k0) {
        if (!wet(k0) || comp[k0] >= 0) continue;
        std::vector<size_t> cells{k0};
        comp[k0] = id;
        bool border = false;
        for (size_t i = 0; i < cells.size(); ++i) {
            const int x = static_cast<int>(cells[i] % kV), z = static_cast<int>(cells[i] / kV);
            if (x == 0 || z == 0 || x == kV - 1 || z == kV - 1) border = true;
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& o : nb) {
                const int xx = x + o[0], zz = z + o[1];
                if (xx < 0 || zz < 0 || xx >= kV || zz >= kV) continue;
                const size_t q = vk(xx, zz);
                if (wet(q) && comp[q] < 0) {
                    comp[q] = id;
                    cells.push_back(q);
                }
            }
        }
        ++id;
        if (border || cells.size() <= best.size()) continue;
        int64_t dmin = INT64_MAX;
        for (size_t c : cells)
            for (const Point& p : road) {
                const int64_t dx = static_cast<int64_t>(c % kV) * kTexel - p.x, dz = static_cast<int64_t>(c / kV) * kTexel - p.z;
                dmin = std::min(dmin, dx * dx + dz * dz);
            }
        const int32_t d = isqrt64(dmin);
        // Off the road, but by the road: the shipped lakes lie 1 800 to 11 000 units from it.
        // CITY1's canal, 44 000 away at the map's edge, is no lake.
        if (d < track_width / 2 + 1500 || d > 16000) continue;
        best = std::move(cells);
        lake.distance = d;
    }
    if (best.empty()) return lake;
    lake.area = static_cast<int32_t>(best.size());
    std::array<int, AutoTiles::kMaxMaterials> wm{};
    int64_t sx = 0, sz = 0;
    for (size_t c : best) {
        ++wm[static_cast<size_t>(V[c])];
        sx += static_cast<int64_t>(c % kV);
        sz += static_cast<int64_t>(c / kV);
    }
    for (int m = 0; m < at.materials; ++m)
        if (wm[static_cast<size_t>(m)] && (lake.water < 0 || wm[static_cast<size_t>(m)] > wm[static_cast<size_t>(lake.water)])) lake.water = m;
    // The elongation, from the second moments (in 64ths of a vertex to keep the integers fine).
    const int64_t n = static_cast<int64_t>(best.size()), mx = sx * 64 / n, mz = sz * 64 / n;
    int64_t cxx = 0, czz = 0, cxz = 0;
    for (size_t c : best) {
        const int64_t dx = static_cast<int64_t>(c % kV) * 64 - mx, dz = static_cast<int64_t>(c / kV) * 64 - mz;
        cxx += dx * dx;
        czz += dz * dz;
        cxz += dx * dz;
    }
    cxx /= n;
    czz /= n;
    cxz /= n;
    const int64_t tr = cxx + czz, disc = isqrt64((cxx - czz) * (cxx - czz) + 4 * cxz * cxz);
    const int64_t l1 = (tr + disc) / 2, l2 = std::max<int64_t>(1, (tr - disc) / 2);
    lake.elongation = static_cast<int>(std::clamp<int64_t>(isqrt64(l1 * 1000000 / l2), 1000, 4000));
    // The shore: rings of vertices round the water (ring r marked r + 1, water 1); the most common
    // ground of the first ring, and how many rings in a row it holds at least half of.
    std::vector<uint8_t> mark(nv, 0);
    for (size_t c : best) mark[c] = 1;
    for (int ring = 1; ring <= 4; ++ring) {
        std::vector<size_t> layer;
        for (size_t k = 0; k < nv; ++k) {
            if (mark[k]) continue;
            const int x = static_cast<int>(k % kV), z = static_cast<int>(k / kV);
            const int r = ring;   // the previous layer's mark
            if ((x > 0 && mark[k - 1] == r) || (x + 1 < kV && mark[k + 1] == r) || (z > 0 && mark[k - kV] == r) ||
                (z + 1 < kV && mark[k + kV] == r))
                layer.push_back(k);
        }
        std::array<int, AutoTiles::kMaxMaterials> count{};
        int land = 0;
        for (size_t k : layer) {
            mark[k] = static_cast<uint8_t>(ring + 1);
            if (V[k] >= 0 && !at.wet[static_cast<size_t>(V[k])]) {
                ++count[static_cast<size_t>(V[k])];
                ++land;
            }
        }
        if (ring == 1) {
            for (int m = 0; m < at.materials; ++m)
                if (count[static_cast<size_t>(m)] && (lake.shore < 0 || count[static_cast<size_t>(m)] > count[static_cast<size_t>(lake.shore)])) lake.shore = m;
            if (lake.shore < 0) break;
        } else if (land > 0 && count[static_cast<size_t>(lake.shore)] * 2 >= land) {
            lake.shore_rings = ring;
        } else {
            break;
        }
    }
    return lake;
}

} // namespace

TrackLook learn_track_look(const AutoTiles& at, const Blob& mpc, const Blob& mph, const std::vector<Point>& road,
                           const std::vector<int32_t>& heading) {
    TrackLook L;
    if (!at.valid() || road.size() < 16 || heading.size() != road.size() ||
        mpc.size() != static_cast<size_t>(kMap) * kMap || mph.size() != mpc.size())
        return L;
    const std::vector<int8_t> V = vertex_materials(at, mpc);
    const std::array<bool, AutoTiles::kMaxMaterials> steep = steep_grounds(at, V, mph);
    // The ground of the vertex nearest a world point, -1 off the map or where no tile says.
    auto mat = [&](int32_t x, int32_t z) -> int {
        if (x < -kTexel / 2 || z < -kTexel / 2) return -1;
        const int vx = (x + kTexel / 2) / kTexel, vz = (z + kTexel / 2) / kTexel;
        if (vx >= kV || vz >= kV) return -1;
        return V[static_cast<size_t>(vz) * kV + vx];
    };
    const int M = at.materials;
    // The track: the most common ground at the road's middle.
    std::array<int, AutoTiles::kMaxMaterials> centre{};
    for (const Point& p : road) {
        const int m = mat(p.x, p.z);
        if (m >= 0) ++centre[static_cast<size_t>(m)];
    }
    for (int m = 0; m < M; ++m)
        if (centre[static_cast<size_t>(m)] > 0 && (L.track < 0 || centre[static_cast<size_t>(m)] > centre[static_cast<size_t>(L.track)])) L.track = m;
    if (L.track < 0) return L;
    L.wet = at.wet[static_cast<size_t>(L.track)];
    // The base: the most common ground of the map, neither water nor the track.
    std::array<int, AutoTiles::kMaxMaterials> all{};
    for (int8_t m : V)
        if (m >= 0) ++all[static_cast<size_t>(m)];
    for (int m = 0; m < M; ++m) {
        if (m == L.track || at.wet[static_cast<size_t>(m)]) continue;
        if (all[static_cast<size_t>(m)] > 0 && (L.base < 0 || all[static_cast<size_t>(m)] > all[static_cast<size_t>(L.base)])) L.base = m;
    }
    if (L.base < 0) return L;

    // Across the road, every other point: the run of track through the middle, then each side's
    // grounds out to the base. Sampled every kStep units at the nearest vertex.
    constexpr int32_t kStep = 128, kReach = 4000, kFar = 8000;
    std::vector<int32_t> widths;
    // A river is ALL its water: WATER2 and WATER3 have two shades, and counting only the one at
    // the middle measured a river one cell wide, the other shade taken for its bank.
    auto on_track = [&](int g) { return g >= 0 && (g == L.track || (L.wet && at.wet[static_cast<size_t>(g)])); };
    std::map<std::vector<int>, std::vector<std::vector<int32_t>>> seqs;   // ordered: deterministic
    int sides = 0;
    const int m = static_cast<int>(road.size());
    for (int i = 0; i < m; i += 2) {
        const Point& c = road[static_cast<size_t>(i)];
        const int32_t t = heading[static_cast<size_t>(i)];
        const int32_t nx = fx::cos16(t), nz = -fx::sin16(t);   // across: the left normal
        auto at_off = [&](int32_t o) { return mat(c.x + fx::mul15(o, nx), c.z + fx::mul15(o, nz)); };
        int32_t o0 = INT32_MAX;
        for (int32_t o = -kReach; o <= kReach; o += kStep)
            if (on_track(at_off(o)) && std::abs(o) < std::abs(o0)) o0 = o;
        if (o0 == INT32_MAX) continue;
        int32_t lo = o0, hi = o0;
        while (lo > -kFar && on_track(at_off(lo - kStep))) lo -= kStep;
        while (hi < kFar && on_track(at_off(hi + kStep))) hi += kStep;
        widths.push_back(hi - lo + kStep);
        for (int side = -1; side <= 1; side += 2) {
            const int32_t edge = side > 0 ? hi : lo;
            std::vector<int> mats;
            std::vector<int32_t> runs;
            bool reached = false;
            for (int32_t e = kStep; e <= kFar; e += kStep) {
                const int g = at_off(edge + side * e);
                if (g < 0) break;
                if (g == L.base) {
                    reached = true;
                    break;
                }
                if (steep[static_cast<size_t>(g)]) continue;   // a slope's ground: never a bank
                if (mats.empty() || mats.back() != g) {
                    mats.push_back(g);
                    runs.push_back(0);
                }
                runs.back() += kStep;
            }
            if (!reached) continue;
            ++sides;
            seqs[mats].push_back(runs);
        }
    }
    if (widths.empty()) return L;
    std::sort(widths.begin(), widths.end());
    L.width = widths[widths.size() / 2];
    L.width_lo = std::max<int32_t>(kTexel, widths[widths.size() / 4]);
    L.width_hi = std::max(L.width, widths[widths.size() * 3 / 4]);
    // A river never narrower than its median: at WATER2's first quartile (512, one vertex of
    // water) no cell is all water, and the INFLATE ran aground on the flat banks.
    if (L.wet) L.width_lo = L.width;
    // The banks: the most common sequence (the shorter on a tie), each band its median width.
    const std::vector<int>* best = nullptr;
    size_t best_n = 0;
    for (const auto& [k, v] : seqs)
        if (v.size() > best_n || (v.size() == best_n && best && k.size() < best->size())) {
            best = &k;
            best_n = v.size();
        }
    if (best) {
        const auto& rows = seqs.at(*best);
        for (size_t b = 0; b < best->size(); ++b) {
            std::vector<int32_t> w;
            for (const auto& r : rows) w.push_back(r[b]);
            std::sort(w.begin(), w.end());
            L.banks.push_back({(*best)[b], w[w.size() / 2]});
        }
        L.bank_share = static_cast<int>(best_n * 1000 / static_cast<size_t>(std::max(sides, 1)));
    }
    L.lake = learn_lake(at, V, road, L.width);
    L.report = "piste " + std::to_string(L.track) + (L.wet ? " (eau)" : "") + " de " + std::to_string(L.width) + " (" +
               std::to_string(L.width_lo) + "-" + std::to_string(L.width_hi) + "), base " + std::to_string(L.base) + ", berges";
    if (L.banks.empty()) L.report += " : aucune";
    for (const TrackLook::Band& b : L.banks) L.report += " " + std::to_string(b.material) + "x" + std::to_string(b.width);
    L.report += " (" + std::to_string(L.bank_share / 10) + " % des bords)";
    if (L.lake.valid())
        L.report += ", lac " + std::to_string(L.lake.water) + " de " + std::to_string(L.lake.area) + " sommets (allonge " +
                    std::to_string(L.lake.elongation) + " pour mille, a " + std::to_string(L.lake.distance) + " de la route), rive " +
                    std::to_string(L.lake.shore) + " sur " + std::to_string(L.lake.shore_rings) + " sommet(s)";
    return L;
}

std::string paint_layers(const TrackLook& look, const std::vector<Point>& centre, const std::vector<int32_t>& width,
                         uint64_t lake_seed, std::vector<int8_t>& V, std::vector<uint8_t>& locked,
                         std::vector<std::vector<uint16_t>>* soft) {
    const size_t nv = static_cast<size_t>(kV) * kV;
    V.assign(nv, static_cast<int8_t>(look.base));
    locked.assign(nv, 0);
    const int n = static_cast<int>(centre.size());
    if (!look.valid() || n < 3 || width.size() != centre.size()) return "rien a peindre";
    // The nearest point of the centre line to every vertex near it, and the track's width there.
    int32_t widest = 0;
    for (int32_t w : width) widest = std::max(widest, w);
    const int32_t reach = widest / 2 + look.banks_width() + kTexel * 2;
    std::vector<int64_t> best(nv, INT64_MAX);
    std::vector<int32_t> w_at(nv, 0);
    for (int i = 0; i < n; ++i) {
        const Point& a = centre[static_cast<size_t>(i)];
        const Point& b = centre[static_cast<size_t>((i + 1) % n)];
        const int32_t wa = width[static_cast<size_t>(i)], wb = width[static_cast<size_t>((i + 1) % n)];
        const int64_t abx = b.x - a.x, abz = b.z - a.z, len2 = abx * abx + abz * abz;
        const int x0 = std::max(0, (std::min(a.x, b.x) - reach) / kTexel), x1 = std::min(kV - 1, (std::max(a.x, b.x) + reach) / kTexel + 1);
        const int z0 = std::max(0, (std::min(a.z, b.z) - reach) / kTexel), z1 = std::min(kV - 1, (std::max(a.z, b.z) + reach) / kTexel + 1);
        for (int vz = z0; vz <= z1; ++vz)
            for (int vx = x0; vx <= x1; ++vx) {
                const int64_t px = static_cast<int64_t>(vx) * kTexel - a.x, pz = static_cast<int64_t>(vz) * kTexel - a.z;
                const int64_t tn = len2 > 0 ? std::clamp<int64_t>(px * abx + pz * abz, 0, len2) : 0;
                const int64_t qx = len2 > 0 ? px - abx * tn / len2 : px, qz = len2 > 0 ? pz - abz * tn / len2 : pz;
                const int64_t d2 = qx * qx + qz * qz;
                const size_t k = static_cast<size_t>(vz) * kV + vx;
                if (d2 < best[k]) {
                    best[k] = d2;
                    w_at[k] = len2 > 0 ? static_cast<int32_t>(wa + (wb - wa) * tn / len2) : wa;
                }
            }
    }

    // THE LAKE, before the track: the donor's size and elongation, a wavy outline, at the donor's
    // distance from the road (or further, so that it never reaches the track's banks).
    std::string report = "pas de lac";
    const LakeLook& lk = look.lake;
    if (lk.valid()) {
        uint64_t st = lake_seed;
        auto draw = [&](int32_t lo, int32_t hi) {   // splitmix64, in [lo, hi]
            uint64_t z = (st += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return hi <= lo ? lo : static_cast<int32_t>(lo + static_cast<int64_t>(z % static_cast<uint64_t>(hi - lo + 1)));
        };
        // Semi-axes in world units: the donor's area as an ellipse of its elongation.
        const int32_t r0 = isqrt64(static_cast<int64_t>(lk.area) * kTexel * kTexel * 1000 / 3142);
        const int32_t sq = isqrt64(static_cast<int64_t>(lk.elongation) * 1000);   // sqrt(e) x 1000
        const int32_t ax = r0 * sq / 1000, bx = r0 * 1000 / std::max(sq, 1);
        constexpr int kWobble = 250;                                                 // per mille, at most
        const int32_t r_long = ax * (1000 + kWobble) / 1000;
        const int32_t ring = lk.shore_rings * kTexel;
        // Nothing of the lake or its shore within this of the centre line: past the banks, with
        // grass between (`reach` covers it, so a vertex the track's pass never reached is clear).
        const int32_t clear = widest / 2 + look.banks_width() + 1000;
        // Candidate centres every other vertex, inside the map, whose distance to the centre line
        // puts the lake's near edge about where the donor's is (its distance, +0 .. +6 000).
        const int margin = (r_long + ring) / kTexel + 12;
        std::vector<size_t> cand;
        for (int vz = margin; vz < kV - margin; vz += 2)
            for (int vx = margin; vx < kV - margin; vx += 2) {
                int64_t d2 = INT64_MAX;
                for (const Point& p : centre) {
                    const int64_t dx = static_cast<int64_t>(vx) * kTexel - p.x, dz = static_cast<int64_t>(vz) * kTexel - p.z;
                    d2 = std::min(d2, dx * dx + dz * dz);
                }
                const int32_t d = isqrt64(d2) - bx;   // the near edge, if the short axis faces the track
                if (d >= std::max(lk.distance, clear) && d <= std::max(lk.distance, clear) + 6000) cand.push_back(static_cast<size_t>(vz) * kV + vx);
            }
        std::vector<uint8_t> water(nv, 0), shore;
        int count = 0, tries = 0;
        int32_t placed_at = -1;
        for (; tries < 60 && !cand.empty() && placed_at < 0; ++tries) {
            const size_t c = cand[static_cast<size_t>(draw(0, static_cast<int32_t>(cand.size()) - 1))];
            const int32_t cx = static_cast<int32_t>(c % kV) * kTexel, cz = static_cast<int32_t>(c / kV) * kTexel;
            const int32_t axis = draw(0, 0xFFFF), ph1 = draw(0, 0xFFFF), ph2 = draw(0, 0xFFFF);
            const int32_t k1 = draw(80, 160), k2 = draw(40, kWobble - 160);
            std::fill(water.begin(), water.end(), 0);
            count = 0;
            const int span = (r_long + ring) / kTexel + 2;
            const int gx = cx / kTexel, gz = cz / kTexel;
            for (int vz = std::max(0, gz - span); vz <= std::min(kV - 1, gz + span); ++vz)
                for (int vx = std::max(0, gx - span); vx <= std::min(kV - 1, gx + span); ++vx) {
                    const int32_t dx = vx * kTexel - cx, dz = vz * kTexel - cz;
                    // Along the long axis (sin, cos of `axis`) and across it.
                    const int64_t u = (static_cast<int64_t>(dx) * fx::sin16(axis) + static_cast<int64_t>(dz) * fx::cos16(axis)) >> 15;
                    const int64_t v = (static_cast<int64_t>(dx) * fx::cos16(axis) - static_cast<int64_t>(dz) * fx::sin16(axis)) >> 15;
                    const int32_t th = fx::atan2_bam16(dx, dz);
                    const int64_t wob = 1000 + (static_cast<int64_t>(k1) * fx::sin16(2 * th + ph1) + static_cast<int64_t>(k2) * fx::sin16(3 * th + ph2)) / 32768;
                    // (u / (ax wob))^2 + (v / (bx wob))^2 <= 1, in integers
                    const int64_t ua = u * 1000 * 1000 / (static_cast<int64_t>(ax) * wob), vb = v * 1000 * 1000 / (static_cast<int64_t>(bx) * wob);
                    if (ua * ua + vb * vb <= int64_t(1000) * 1000) {
                        water[vk(vx, vz)] = 1;
                        ++count;
                    }
                }
            // The outline smoothed as a painter's: no water vertex with one water neighbour or
            // none (a spike), no dry vertex with three or four (a notch).
            for (int pass = 0; pass < 2; ++pass) {
                std::vector<uint8_t> next = water;
                for (int vz = 1; vz + 1 < kV; ++vz)
                    for (int vx = 1; vx + 1 < kV; ++vx) {
                        const size_t k = vk(vx, vz);
                        const int nb = water[k - 1] + water[k + 1] + water[k - kV] + water[k + kV];
                        if (water[k] && nb <= 1) next[k] = 0;
                        if (!water[k] && nb >= 3) next[k] = 1;
                    }
                water.swap(next);
            }
            count = 0;
            for (uint8_t w : water) count += w;
            // The shore: shore_rings vertices round the water, by 4-neighbour steps.
            shore = water;
            for (int r = 0; r < lk.shore_rings; ++r) {
                std::vector<uint8_t> next = shore;
                for (int vz = 0; vz < kV; ++vz)
                    for (int vx = 0; vx < kV; ++vx) {
                        const size_t k = vk(vx, vz);
                        if (shore[k]) continue;
                        if ((vx > 0 && shore[k - 1]) || (vx + 1 < kV && shore[k + 1]) || (vz > 0 && shore[k - kV]) ||
                            (vz + 1 < kV && shore[k + kV]))
                            next[k] = 1;
                    }
                shore.swap(next);
            }
            bool ok = count > 0;
            int32_t nearest = INT32_MAX;
            for (size_t k = 0; k < nv && ok; ++k)
                if (shore[k] && best[k] != INT64_MAX) {
                    const int32_t d = isqrt64(best[k]);
                    nearest = std::min(nearest, d);
                    ok = d > clear;
                }
            if (ok) placed_at = nearest == INT32_MAX ? reach : nearest;
        }
        if (placed_at < 0) {
            report = "pas de place pour le lac";
        } else {
            for (size_t k = 0; k < nv; ++k) {
                if (water[k]) {
                    V[k] = static_cast<int8_t>(lk.water);
                    locked[k] = 1;
                } else if (shore[k]) {
                    V[k] = static_cast<int8_t>(lk.shore);
                }
            }
            report = "lac de " + std::to_string(count) + " sommets, sa rive a " + std::to_string(placed_at) + " de la piste (" +
                     std::to_string(tries) + " essai(s))";
        }
    }

    // THE TRACK and its banks, over everything. Each vertex first gets its LEVEL - 0 the track,
    // 1.. the banks outwards, `nb + 1` untouched - from its distance to the centre line.
    const int nb = static_cast<int>(look.banks.size());
    std::vector<int8_t> level(nv, static_cast<int8_t>(nb + 1));
    std::vector<int32_t> dist(nv, INT32_MAX);   // to the centre line, where the track's pass reached
    for (size_t k = 0; k < nv; ++k) {
        if (best[k] == INT64_MAX) continue;
        const int32_t d = isqrt64(best[k]);
        dist[k] = d;
        int32_t edge = w_at[k] / 2;
        int lv = 0;
        while (lv <= nb && d > edge) {
            if (lv < nb) edge += look.banks[static_cast<size_t>(lv)].width;
            ++lv;
        }
        level[k] = static_cast<int8_t>(lv);
    }
    // Each limit smoothed as a painter draws it (user, 2026-10-04: "les bords de la piste sont
    // souvent en escalier"): where the width changes, or the distance threshold falls between two
    // vertices, the edge stepped out by one vertex for a single row and back - a lone spike - or
    // in, a notch; the shipped edges always move one way along a stretch. So a vertex inside a
    // limit with one neighbour inside or none leaves it, a vertex outside with three or four
    // comes in; twice, for every limit from the track outwards.
    for (int lim = 1; lim <= nb + 1; ++lim)
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<int8_t> next = level;
            for (int vz = 1; vz + 1 < kV; ++vz)
                for (int vx = 1; vx + 1 < kV; ++vx) {
                    const size_t k = vk(vx, vz);
                    auto in = [&](size_t q) { return level[q] < lim ? 1 : 0; };
                    const int n4 = in(k - 1) + in(k + 1) + in(k - kV) + in(k + kV);
                    if (in(k) && n4 <= 1) next[k] = static_cast<int8_t>(lim);
                    else if (!in(k) && n4 >= 3) next[k] = static_cast<int8_t>(lim - 1);
                }
            level.swap(next);
        }
    const std::vector<int8_t> raw = level;   // before smoothing: what the distance alone says
    for (size_t k = 0; k < nv; ++k) {
        if (level[k] == 0) {
            V[k] = static_cast<int8_t>(look.track);
            locked[k] = 1;
        } else if (level[k] <= nb) {
            V[k] = static_cast<int8_t>(look.banks[static_cast<size_t>(level[k] - 1)].material);
        }
    }
    // THE TRUE EDGES, for the autotiler's edge variants: each layer's membership ramps from 0 to
    // 1 000 over one vertex spacing centred on its true edge. Only where the vertex still has the
    // layer its distance gives it (the smoothing moved some) and is not the lake's.
    if (soft) {
        soft->assign(AutoTiles::kMaxMaterials, std::vector<uint16_t>(nv, kSoftUnknown));
        auto ramp = [](int32_t inside) {   // inside: units inside the edge (negative: outside)
            return static_cast<uint16_t>(std::clamp<int64_t>(500 + int64_t(inside) * 1000 / kTexel, 0, 1000));
        };
        for (size_t k = 0; k < nv; ++k) {
            if (dist[k] == INT32_MAX || raw[k] != level[k]) continue;
            const int lv = level[k];
            const int m_here = lv == 0 ? look.track : lv <= nb ? look.banks[static_cast<size_t>(lv - 1)].material : look.base;
            if (V[k] != m_here) continue;   // the lake, or the repair's
            const int32_t d = dist[k];
            int32_t inner = INT32_MIN / 2, outer = w_at[k] / 2;   // the layer's edges, from the centre line
            std::vector<std::pair<int, uint16_t>> mem;   // ground, membership
            for (int l = 0; l <= nb + 1; ++l) {
                if (l > 0) {
                    inner = outer;
                    outer = l <= nb ? outer + look.banks[static_cast<size_t>(l - 1)].width : INT32_MAX / 2;
                }
                const int m = l == 0 ? look.track : l <= nb ? look.banks[static_cast<size_t>(l - 1)].material : look.base;
                const uint16_t v = std::min(l == 0 ? static_cast<uint16_t>(1000) : ramp(d - inner), l == nb + 1 ? static_cast<uint16_t>(1000) : ramp(outer - d));
                mem.push_back({m, v});
            }
            // A ground may come back in two layers (SNOW's bank and base): its best.
            for (const auto& [m, v] : mem) {
                uint16_t& s = (*soft)[static_cast<size_t>(m)][k];
                s = s == kSoftUnknown ? v : std::max(s, v);
            }
            for (int m = 0; m < AutoTiles::kMaxMaterials; ++m) {
                uint16_t& s = (*soft)[static_cast<size_t>(m)][k];
                if (s == kSoftUnknown) s = 0;
            }
        }
    }
    return report;
}

} // namespace bigred::gen
