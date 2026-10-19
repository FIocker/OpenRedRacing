// The autotiler. See the header.
// The prototype, with pictures of the round trip, is tools/autotile_study.py.

#include "gen/autotile.h"

#include "formats/terrain.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace bigred::gen {

namespace {

constexpr int kMap = 256;
constexpr int kV = kMap + 1;          // vertices per side
constexpr int kTile = 32;
constexpr int kPic = AutoTiles::kPic;
constexpr int kMaxM = AutoTiles::kMaxMaterials;
constexpr int kBlurSide = 1, kBlurMid = 3;   // the field's vertex blur [1 3 1]^2: sigma ~0.63 cell
constexpr int kBlurSum = 2 * kBlurSide + kBlurMid;
constexpr int kField = kBlurSum * kBlurSum * 256;   // 1.0 in the boundary field (blur, then bilinear /256)
constexpr int kNoTile = -2;           // vertex_materials' "no tile here" is -1

size_t vk(int x, int z) { return static_cast<size_t>(z) * kV + x; }
size_t ck(int x, int z) { return static_cast<size_t>(z) * kMap + x; }

// The four cells round vertex (x,z), and which of their corners it is.
struct Around { int dx, dz, corner; };
constexpr Around kAround[4] = {{-1, -1, 3}, {0, -1, 2}, {-1, 0, 1}, {0, 0, 0}};
constexpr int kCornerDx[4] = {0, 1, 0, 1};
constexpr int kCornerDz[4] = {0, 0, 1, 1};

// Nearest multiple of 45 degrees of vector (X,Y), 0..7 counter-clockwise from +X; -1 for zero.
// tan(22.5) ~ 106/256.
int octant(int64_t X, int64_t Y) {
    if (X == 0 && Y == 0) return -1;
    const int64_t ax = X < 0 ? -X : X, ay = Y < 0 ? -Y : Y;
    if (ay * 256 <= ax * 106) return X >= 0 ? 0 : 4;
    if (ax * 256 <= ay * 106) return Y >= 0 ? 2 : 6;
    if (X > 0) return Y > 0 ? 1 : 7;
    return Y > 0 ? 3 : 5;
}

int step_distance(int a, int b) {
    const int d = std::abs(a - b) % 8;
    return std::min(d, 8 - d);
}

// Separable binomial blur of a w x h int64 grid, edges clamped.
void blur(std::vector<int64_t>& g, int w, int h, const std::vector<int64_t>& kernel) {
    const int r = static_cast<int>(kernel.size()) / 2;
    std::vector<int64_t> t(g.size());
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            int64_t s = 0;
            for (int i = -r; i <= r; ++i) s += kernel[static_cast<size_t>(i + r)] * g[static_cast<size_t>(z) * w + std::clamp(x + i, 0, w - 1)];
            t[static_cast<size_t>(z) * w + x] = s;
        }
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            int64_t s = 0;
            for (int i = -r; i <= r; ++i) s += kernel[static_cast<size_t>(i + r)] * t[static_cast<size_t>(std::clamp(z + i, 0, h - 1)) * w + x];
            g[static_cast<size_t>(z) * w + x] = s;
        }
}

std::vector<int64_t> binomial(int n) {
    std::vector<int64_t> k(static_cast<size_t>(n) + 1, 1);
    for (int i = 1; i < n; ++i) k[static_cast<size_t>(i)] = k[static_cast<size_t>(i) - 1] * (n - i + 1) / i;
    return k;
}

// Per cell, the direction of the strip of vertices of material m, 0..7 (x 22.5 degrees, x
// towards z), -1 where there is none: the structure tensor of the smoothed strip, whose weaker
// axis runs along it. sigma 1 then 2 cells, as the prototype.
std::vector<int8_t> strip_steps(const std::vector<int8_t>& V, int m) {
    std::vector<int64_t> cell(static_cast<size_t>(kMap) * kMap);
    for (int z = 0; z < kMap; ++z)
        for (int x = 0; x < kMap; ++x)
            cell[ck(x, z)] = (V[vk(x, z)] == m) + (V[vk(x + 1, z)] == m) + (V[vk(x, z + 1)] == m) + (V[vk(x + 1, z + 1)] == m);
    blur(cell, kMap, kMap, binomial(4));
    std::vector<int64_t> jxx(cell.size()), jzz(cell.size()), jxz(cell.size());
    for (int z = 0; z < kMap; ++z)
        for (int x = 0; x < kMap; ++x) {
            const int64_t gx = cell[ck(std::min(x + 1, kMap - 1), z)] - cell[ck(std::max(x - 1, 0), z)];
            const int64_t gz = cell[ck(x, std::min(z + 1, kMap - 1))] - cell[ck(x, std::max(z - 1, 0))];
            jxx[ck(x, z)] = gx * gx;
            jzz[ck(x, z)] = gz * gz;
            jxz[ck(x, z)] = gx * gz;
        }
    // values up to ~2e6 per product; the 17-tap blur multiplies by 65536^2 over two passes:
    // scale down first to stay inside int64.
    for (auto* g : {&jxx, &jzz, &jxz})
        for (int64_t& v : *g) v >>= 8;
    const std::vector<int64_t> k16 = binomial(16);
    blur(jxx, kMap, kMap, k16);
    blur(jzz, kMap, kMap, k16);
    blur(jxz, kMap, kMap, k16);
    std::vector<int8_t> out(cell.size());
    for (size_t i = 0; i < out.size(); ++i)
        // the gradient's doubled angle is atan2(2 jxz, jxx - jzz); the strip's is that + 180
        out[i] = static_cast<int8_t>(octant(-(jxx[i] - jzz[i]), -2 * jxz[i]));
    return out;
}

bool wet(const AutoTiles& at, int m) { return m >= 0 && at.wet[static_cast<size_t>(m)]; }

std::vector<uint8_t> ring_of(const AutoTiles& at, const std::vector<int8_t>& V) {
    std::vector<uint8_t> r(V.size(), 0);
    if (at.water < 0) return r;
    for (int z = 0; z < kV; ++z)
        for (int x = 0; x < kV; ++x) {
            if (wet(at, V[vk(x, z)])) continue;
            const bool w = (x > 0 && wet(at, V[vk(x - 1, z)])) || (x + 1 < kV && wet(at, V[vk(x + 1, z)])) ||
                           (z > 0 && wet(at, V[vk(x, z - 1)])) || (z + 1 < kV && wet(at, V[vk(x, z + 1)]));
            r[vk(x, z)] = w ? 1 : 0;
        }
    return r;
}

// A cell with water within a vertex of it: its dirt runs across the strip, not along.
std::vector<uint8_t> near_water(const AutoTiles& at, const std::vector<int8_t>& V) {
    std::vector<uint8_t> n(static_cast<size_t>(kMap) * kMap, 0);
    if (at.water < 0) return n;
    for (int z = 0; z < kMap; ++z)
        for (int x = 0; x < kMap; ++x)
            for (int dz = -1; dz <= 2 && !n[ck(x, z)]; ++dz)
                for (int dx = -1; dx <= 2; ++dx) {
                    const int vx = x + dx, vz = z + dz;
                    if (vx >= 0 && vz >= 0 && vx < kV && vz < kV && wet(at, V[vk(vx, vz)])) {
                        n[ck(x, z)] = 1;
                        break;
                    }
                }
    return n;
}

struct VariantCell { int x, z; std::vector<int> cand; };

// The boundary field of one material at one cell: its vertex indicator blurred [1 3 1]^2, then
// sampled bilinearly at the kPic x kPic pixel centres. 0..kField.
struct Field {
    std::array<std::vector<int16_t>, kMaxM> blurred;   // per material, per vertex, 0..kBlurSum^2
    void build(const std::vector<int8_t>& V, int materials) {
        for (int m = 0; m < materials; ++m) {
            std::vector<int64_t> g(V.size());
            for (size_t i = 0; i < V.size(); ++i) g[i] = V[i] == m;
            blur(g, kV, kV, {kBlurSide, kBlurMid, kBlurSide});
            blurred[static_cast<size_t>(m)].assign(g.begin(), g.end());
        }
    }
    int at(int m, int x, int z, int px, int pz) const {
        const auto& b = blurred[static_cast<size_t>(m)];
        const int wx1 = 2 * px + 1, wx0 = 16 - wx1, wz1 = 2 * pz + 1, wz0 = 16 - wz1;
        return (b[vk(x, z)] * wx0 + b[vk(x + 1, z)] * wx1) * wz0 + (b[vk(x, z + 1)] * wx0 + b[vk(x + 1, z + 1)] * wx1) * wz1;
    }
};


int pick_variant(const AutoTiles& at, const Field& f, const std::array<int, kMaxM>& bias, const std::array<int8_t, 4>& cr,
                 int x, int z, const std::vector<int>& cand) {
    int ms[4], nm = 0;
    for (int8_t m : cr)
        if (std::find(ms, ms + nm, m) == ms + nm) ms[nm++] = m;
    std::sort(ms, ms + nm);
    uint8_t tgt[kPic * kPic];
    for (int pz = 0; pz < kPic; ++pz)
        for (int px = 0; px < kPic; ++px) {
            int best = ms[0], bv = f.at(ms[0], x, z, px, pz) + bias[static_cast<size_t>(ms[0])];
            for (int i = 1; i < nm; ++i) {
                const int v = f.at(ms[i], x, z, px, pz) + bias[static_cast<size_t>(ms[i])];
                if (v > bv) { bv = v; best = ms[i]; }
            }
            tgt[pz * kPic + px] = static_cast<uint8_t>(best);
        }
    int pick = -1, pick_miss = 0;
    for (int code : cand) {
        int miss = 0;
        for (int i = 0; i < kPic * kPic; ++i) miss += at.picture[static_cast<size_t>(code)][static_cast<size_t>(i)] != tgt[i];
        if (pick < 0 || miss < pick_miss || (miss == pick_miss && at.count[static_cast<size_t>(code)] > at.count[static_cast<size_t>(pick)])) {
            pick = code;
            pick_miss = miss;
        }
    }
    return pick;
}

// The laying itself. With `variants` set, the cells with several candidates are listed there
// instead of decided (fit_bias scores them itself).
int lay(const AutoTiles& at, const std::array<int, kMaxM>& bias, const std::vector<int8_t>& V, Blob& mpc,
        const std::vector<uint8_t>& keep, std::vector<VariantCell>* variants, bool sow,
        const std::vector<std::vector<uint16_t>>* soft = nullptr) {
    // candidates by corners
    std::vector<int> used;
    for (int c = 0; c < 256; ++c) {
        const auto& q = at.corners[static_cast<size_t>(c)];
        if (at.tile[static_cast<size_t>(c)] && at.count[static_cast<size_t>(c)] > 0 && q[0] >= 0 && q[1] >= 0 && q[2] >= 0 && q[3] >= 0)
            used.push_back(c);
    }
    const std::vector<uint8_t> R = ring_of(at, V);
    const std::vector<uint8_t> near = near_water(at, V);
    std::array<std::vector<int8_t>, kMaxM> strips;
    for (int m = 0; m < at.materials; ++m)
        if (at.oriented[static_cast<size_t>(m)]) strips[static_cast<size_t>(m)] = strip_steps(V, m);
    Field field;
    if (!variants) {
        field.build(V, at.materials);
        if (soft)
            for (int m = 0; m < at.materials && m < static_cast<int>(soft->size()); ++m) {
                const std::vector<uint16_t>& s = (*soft)[static_cast<size_t>(m)];
                if (s.size() != V.size()) continue;
                for (size_t v = 0; v < V.size(); ++v)
                    if (s[v] != kSoftUnknown) field.blurred[static_cast<size_t>(m)][v] = static_cast<int16_t>(s[v] * kBlurSum * kBlurSum / 1000);
            }
    }
    int changed = 0;
    for (int z = 0; z < kMap; ++z)
        for (int x = 0; x < kMap; ++x) {
            const size_t k = ck(x, z);
            if (!at.tile[mpc[k]] || (!keep.empty() && keep[k])) continue;
            const std::array<int8_t, 4> cr = {V[vk(x, z)], V[vk(x + 1, z)], V[vk(x, z + 1)], V[vk(x + 1, z + 1)]};
            if (cr[0] < 0 || cr[1] < 0 || cr[2] < 0 || cr[3] < 0) continue;
            int pick = -1;
            if (cr[0] == cr[1] && cr[1] == cr[2] && cr[2] == cr[3]) {
                const int m = cr[0];
                pick = at.fill[static_cast<size_t>(m)];
                const AutoTiles::Motif& mo = at.motif[static_cast<size_t>(m)];
                if (mo.rows && mo.row[static_cast<size_t>(z)] >= 0 && mo.col[static_cast<size_t>(x)] >= 0) {
                    const int c = mo.code[static_cast<size_t>(mo.row[static_cast<size_t>(z)]) * mo.cols + mo.col[static_cast<size_t>(x)]];
                    if (c >= 0) pick = c;
                } else if (sow && !mo.rows && !at.oriented[static_cast<size_t>(m)]) {
                    // variants sown at random, as CHOPPER sows its grass and rock: weighted by
                    // how often the source uses them, drawn from the cell's position
                    int64_t total = 0;
                    for (int c = 0; c < 256; ++c)
                        if (at.plain_of[static_cast<size_t>(c)] == m) total += at.count[static_cast<size_t>(c)];
                    const uint32_t hsh = (static_cast<uint32_t>(x) * 2654435761u) ^ (static_cast<uint32_t>(z) * 2246822519u);
                    int64_t r = total > 0 ? static_cast<int64_t>((hsh >> 7) % static_cast<uint32_t>(total)) : -1;
                    for (int c = 0; c < 256 && r >= 0; ++c)
                        if (at.plain_of[static_cast<size_t>(c)] == m && (r -= at.count[static_cast<size_t>(c)]) < 0) pick = c;
                }
                const int s = at.oriented[static_cast<size_t>(m)] ? strips[static_cast<size_t>(m)][k] : -1;
                if (s >= 0) {
                    const int want = (s + (near[k] ? 4 : 0)) % 8;
                    int best_d = 9;
                    for (int c : used) {
                        const auto& q = at.corners[static_cast<size_t>(c)];
                        if (q[0] != m || q[1] != m || q[2] != m || q[3] != m || at.angle[static_cast<size_t>(c)] < 0) continue;
                        const int d = step_distance(want, at.angle[static_cast<size_t>(c)]);
                        if (d < best_d || (d == best_d && at.count[static_cast<size_t>(c)] > at.count[static_cast<size_t>(pick)])) {
                            best_d = d;
                            pick = c;
                        }
                    }
                }
            } else {
                std::vector<int> cand;
                for (int c : used)
                    if (at.corners[static_cast<size_t>(c)] == cr) cand.push_back(c);
                if (cand.size() > 1) {   // the water's one-vertex ring takes its own family
                    std::vector<int> same;
                    for (int c : cand) {
                        bool ok = true;
                        for (int i = 0; i < 4; ++i)
                            if (!wet(at, cr[static_cast<size_t>(i)]) && at.ring[static_cast<size_t>(c)][static_cast<size_t>(i)] != (R[vk(x + kCornerDx[i], z + kCornerDz[i])] != 0))
                                ok = false;
                        if (ok) same.push_back(c);
                    }
                    if (!same.empty()) cand = same;
                }
                // Not a curiosity: a variant the source lays fewer than 3 times is left out
                // (DIRT3's 58, a terrace step filed among its road's edges, twice, drew light
                // triangles along a generated road; DIRT2's rarest real variants are laid 3 times).
                if (cand.size() > 1) {
                    std::vector<int> common;
                    for (int c : cand)
                        if (at.count[static_cast<size_t>(c)] >= 3) common.push_back(c);
                    if (!common.empty()) cand = common;
                }
                if (cand.size() == 1) {
                    pick = cand[0];
                } else if (cand.size() > 1) {
                    if (variants) {
                        variants->push_back({x, z, cand});
                        continue;
                    }
                    pick = pick_variant(at, field, bias, cr, x, z, cand);
                } else {   // missing from the atlas: the fewest corners wrong, the most used
                    int best_miss = 5;
                    for (int c : used) {
                        int miss = 0;
                        for (int i = 0; i < 4; ++i) miss += at.corners[static_cast<size_t>(c)][static_cast<size_t>(i)] != cr[static_cast<size_t>(i)];
                        if (miss < best_miss || (miss == best_miss && at.count[static_cast<size_t>(c)] > at.count[static_cast<size_t>(pick)])) {
                            best_miss = miss;
                            pick = c;
                        }
                    }
                }
            }
            if (pick >= 0 && pick != mpc[k]) {
                mpc[k] = static_cast<uint8_t>(pick);
                ++changed;
            }
        }
    return changed;
}

// Classes of identical lines of a ground (`at(line, i)`: the code, -1 where the line is not of
// it): each line joins the first class it agrees with on 80 % of their common cells (at least 4),
// or starts one. Writes each line's class (-1 for a line with under 4 cells), returns the count.
template <class At>
int line_classes(At at, std::array<int16_t, 256>& out) {
    std::vector<std::array<int16_t, 256>> reps;
    for (int line = 0; line < kMap; ++line) {
        std::array<int16_t, 256> L;
        int valid = 0;
        for (int i = 0; i < kMap; ++i) {
            L[static_cast<size_t>(i)] = static_cast<int16_t>(at(line, i));
            valid += L[static_cast<size_t>(i)] >= 0;
        }
        out[static_cast<size_t>(line)] = -1;
        if (valid < 4) continue;
        int best = -1;
        int64_t best_num = 4, best_den = 5;   // at least 80 %
        for (size_t r = 0; r < reps.size(); ++r) {
            int both = 0, same = 0;
            for (int i = 0; i < kMap; ++i)
                if (L[static_cast<size_t>(i)] >= 0 && reps[r][static_cast<size_t>(i)] >= 0) {
                    ++both;
                    same += L[static_cast<size_t>(i)] == reps[r][static_cast<size_t>(i)];
                }
            if (both >= 4 && same * best_den >= both * best_num) {
                best = static_cast<int>(r);
                best_num = same;
                best_den = both;
                if (same == both) break;
            }
        }
        if (best < 0) {
            reps.push_back(L);
            out[static_cast<size_t>(line)] = static_cast<int16_t>(reps.size() - 1);
        } else {
            auto& R = reps[static_cast<size_t>(best)];
            for (int i = 0; i < kMap; ++i)
                if (R[static_cast<size_t>(i)] < 0) R[static_cast<size_t>(i)] = L[static_cast<size_t>(i)];
            out[static_cast<size_t>(line)] = static_cast<int16_t>(best);
        }
    }
    return static_cast<int>(reps.size());
}

// Gives a class to the lines the source never showed the ground on (-1), so that a generated map
// paints the motif everywhere: the period of the seen lines if they have one (MOON1's rows follow
// z modulo 4), else a class drawn from the seen lines in their proportions, by the line's index
// (MOON1's columns are drawn per column). Without it MOON's stripes stopped at the rectangle
// where MOON1 has its plain, and the plain tile alone filled the rest.
void extend_classes(std::array<int16_t, 256>& cls) {
    for (int p = 1; p <= 8; ++p) {
        bool ok = true;
        int pairs = 0;
        for (int i = 0; i + p < kMap && ok; ++i)
            if (cls[static_cast<size_t>(i)] >= 0 && cls[static_cast<size_t>(i + p)] >= 0) {
                ++pairs;
                ok = cls[static_cast<size_t>(i)] == cls[static_cast<size_t>(i + p)];
            }
        if (!ok || pairs < 8) continue;
        for (int i = 0; i < kMap; ++i)
            for (int j = i % p; j < kMap && cls[static_cast<size_t>(i)] < 0; j += p)
                if (cls[static_cast<size_t>(j)] >= 0) cls[static_cast<size_t>(i)] = cls[static_cast<size_t>(j)];
        break;
    }
    std::vector<int16_t> seen;
    for (int16_t c : cls)
        if (c >= 0) seen.push_back(c);
    if (seen.empty()) return;
    for (int i = 0; i < kMap; ++i)
        if (cls[static_cast<size_t>(i)] < 0)
            cls[static_cast<size_t>(i)] = seen[((static_cast<uint32_t>(i) * 2654435761u) >> 8) % seen.size()];
}

} // namespace

std::vector<int8_t> vertex_materials(const AutoTiles& at, const Blob& mpc) {
    std::vector<int8_t> V(static_cast<size_t>(kV) * kV, -1);
    for (int z = 0; z < kV; ++z)
        for (int x = 0; x < kV; ++x) {
            // the most votes; a tie goes to the first met, in kAround's order
            int order[4], votes[4], n = 0;
            for (const Around& a : kAround) {
                const int cx = x + a.dx, cz = z + a.dz;
                if (cx < 0 || cz < 0 || cx >= kMap || cz >= kMap) continue;
                const uint8_t code = mpc[ck(cx, cz)];
                if (!at.tile[code]) continue;
                const int m = at.corners[code][static_cast<size_t>(a.corner)];
                if (m < 0) continue;
                int i = 0;
                while (i < n && order[i] != m) ++i;
                if (i == n) { order[n] = m; votes[n++] = 0; }
                ++votes[i];
            }
            int best = -1;
            for (int i = 0; i < n; ++i)
                if (best < 0 || votes[i] > votes[best]) best = i;
            if (best >= 0) V[vk(x, z)] = static_cast<int8_t>(order[best]);
        }
    return V;
}

int lay_autotiles(const AutoTiles& at, const std::vector<int8_t>& V, Blob& mpc, const std::vector<uint8_t>& keep, bool sow,
                  const std::vector<std::vector<uint16_t>>* soft) {
    if (!at.valid() || mpc.size() != static_cast<size_t>(kMap) * kMap || V.size() != static_cast<size_t>(kV) * kV) return 0;
    static const std::array<int, kMaxM> kNoBias{};
    return lay(at, soft ? kNoBias : at.bias, V, mpc, keep, nullptr, sow, soft);
}

int repair_materials(const AutoTiles& at, std::vector<int8_t>& V, const std::vector<uint8_t>& locked) {
    if (!at.valid() || V.size() != static_cast<size_t>(kV) * kV) return 0;
    const int M = at.materials;
    auto touch = [&](int a, int b) { return a < 0 || b < 0 || at.touch[static_cast<size_t>(a)][static_cast<size_t>(b)]; };
    // step[a][b]: the ground after a on the shortest chain of transitions from a to b
    std::array<std::array<int8_t, kMaxM>, kMaxM> step;
    for (int a = 0; a < M; ++a) {
        std::array<int, kMaxM> first;
        first.fill(-1);
        std::vector<int> q = {a};
        std::array<bool, kMaxM> seen{};
        seen[static_cast<size_t>(a)] = true;
        for (size_t i = 0; i < q.size(); ++i)
            for (int b = 0; b < M; ++b)
                if (!seen[static_cast<size_t>(b)] && at.touch[static_cast<size_t>(q[i])][static_cast<size_t>(b)]) {
                    seen[static_cast<size_t>(b)] = true;
                    first[static_cast<size_t>(b)] = q[i] == a ? b : first[static_cast<size_t>(q[i])];
                    q.push_back(b);
                }
        for (int b = 0; b < M; ++b) step[static_cast<size_t>(a)][static_cast<size_t>(b)] = static_cast<int8_t>(first[static_cast<size_t>(b)]);
    }
    std::vector<uint8_t> held(V.size(), 0);
    if (!locked.empty()) {
        for (size_t v = 0; v < V.size(); ++v) held[v] = locked[v] ? 1 : 0;
    } else {   // nothing painted: of each clashing pair, the first in scan order holds
        for (int z = 0; z < kV; ++z)
            for (int x = 0; x < kV; ++x)
                for (int dz = 0; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dz && dx <= 0) continue;
                        const int nx = x + dx, nz = z + dz;
                        if (nx < 0 || nx >= kV || nz >= kV) continue;
                        if (!touch(V[vk(x, z)], V[vk(nx, nz)]) && !held[vk(nx, nz)]) held[vk(x, z)] = 1;
                    }
    }
    // Two grounds with no tile of their own may still meet ACROSS a cell when the atlas has a tile
    // for exactly its four corners: DIRT2's 156, 157, 164, 165 (water, paved dirt and grass), the
    // diagonal step of a one-vertex ring. Without this, every grass vertex diagonal to the water
    // turned to dirt, and a generated lake wore lumps of smooth dirt round its paved ring.
    auto cell_has_tile = [&](int cx, int cz) {
        const std::array<int8_t, 4> cr = {V[vk(cx, cz)], V[vk(cx + 1, cz)], V[vk(cx, cz + 1)], V[vk(cx + 1, cz + 1)]};
        for (int c = 0; c < 256; ++c)
            if (at.tile[static_cast<size_t>(c)] && at.count[static_cast<size_t>(c)] >= 3 && at.corners[static_cast<size_t>(c)] == cr) return true;
        return false;
    };
    // each pass, a free vertex against a held one it cannot touch takes the next ground of the
    // chain and holds in turn: the ring grows outwards from the paint, one vertex a pass
    int changed = 0;
    for (int pass = 0; pass < kV; ++pass) {
        std::vector<std::pair<size_t, int8_t>> moves;
        for (int z = 0; z < kV; ++z)
            for (int x = 0; x < kV; ++x) {
                const size_t v = vk(x, z);
                if (held[v] || V[v] < 0) continue;
                int want = -1;
                for (int dz = -1; dz <= 1 && want < 0; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, nz = z + dz;
                        if ((!dx && !dz) || nx < 0 || nz < 0 || nx >= kV || nz >= kV) continue;
                        const size_t u = vk(nx, nz);
                        if (held[u] && !touch(V[u], V[v])) {
                            if (dx && dz && cell_has_tile(std::min(x, nx), std::min(z, nz))) continue;
                            want = step[static_cast<size_t>(V[u])][static_cast<size_t>(V[v])];
                            break;
                        }
                    }
                if (want >= 0) moves.push_back({v, static_cast<int8_t>(want)});
            }
        if (moves.empty()) break;
        for (const auto& [v, m] : moves) {
            changed += V[v] != m;
            V[v] = m;
            held[v] = 1;
        }
    }
    return changed;
}

int thicken_rings(const AutoTiles& at, std::vector<int8_t>& V, const std::vector<uint8_t>& locked) {
    if (!at.valid() || at.water < 0 || V.size() != static_cast<size_t>(kV) * kV) return 0;
    int changed = 0;
    for (int pass = 0; pass < 4; ++pass) {
        const std::vector<uint8_t> R = ring_of(at, V);
        std::vector<std::pair<size_t, int8_t>> moves;
        for (int z = 0; z < kMap; ++z)
            for (int x = 0; x < kMap; ++x) {
                std::array<int8_t, 4> cr;
                std::array<bool, 4> rg;
                int ring_m = -1;
                bool any_wet = false;
                for (int i = 0; i < 4; ++i) {
                    const size_t v = vk(x + kCornerDx[i], z + kCornerDz[i]);
                    cr[static_cast<size_t>(i)] = V[v];
                    rg[static_cast<size_t>(i)] = R[v] != 0;
                    if (V[v] < 0) any_wet = true;   // unknown: leave the cell alone
                    if (wet(at, V[v])) any_wet = true;
                    if (R[v]) ring_m = V[v];
                }
                // Only the ring's outer cells: a ringed corner, no water, two grounds.
                if (any_wet || ring_m < 0 || (cr[0] == cr[1] && cr[1] == cr[2] && cr[2] == cr[3])) continue;
                bool exists = false;
                for (int c = 0; c < 256 && !exists; ++c) {
                    if (!at.tile[static_cast<size_t>(c)] || at.count[static_cast<size_t>(c)] < 3 || at.corners[static_cast<size_t>(c)] != cr) continue;
                    bool ok = true;
                    for (int i = 0; i < 4; ++i)
                        if (at.ring[static_cast<size_t>(c)][static_cast<size_t>(i)] != rg[static_cast<size_t>(i)]) ok = false;
                    exists = ok;
                }
                if (exists) continue;
                for (int i = 0; i < 4; ++i) {
                    const size_t v = vk(x + kCornerDx[i], z + kCornerDz[i]);
                    if (!R[v] && V[v] != ring_m && (locked.empty() || !locked[v])) moves.push_back({v, static_cast<int8_t>(ring_m)});
                }
            }
        if (moves.empty()) break;
        for (const auto& [v, m] : moves) {
            changed += V[v] != m;
            V[v] = m;
        }
    }
    return changed;
}

AutoTiles learn_autotiles(const Archive& level, const std::string& level_name) {
    AutoTiles at;
    for (auto& c : at.corners) c = {-1, -1, -1, -1};
    at.angle.fill(-1);
    at.plain_of.fill(-1);
    const Blob* mpc = find_member(level, "MAP.MPC");
    const Blob* cht = find_member(level, "MAP.CHT");
    const Blob* col = find_member(level, "MAP.COL");
    const Blob* txt = find_member(level, level_name + ".TXT");
    if (!mpc || !cht || !col || !txt || mpc->size() != static_cast<size_t>(kMap) * kMap || cht->size() < 768 || col->size() < 768)
        return at;
    const std::optional<TexesDirective> texes = parse_texes_directive(std::string(txt->begin(), txt->end()));
    if (!texes) return at;
    std::vector<const Blob*> atlases;
    for (const std::string& nm : texes_atlas_filenames(*texes)) {
        const Blob* a = find_member(level, nm + ".TM");
        if (!a || a->size() < 256 * 192) break;
        atlases.push_back(a);
    }
    if (atlases.empty()) return at;
    const Blob& map = *mpc;
    for (uint8_t c : map) ++at.count[c];
    for (int c = 0; c < 256; ++c) {
        const int t = (*cht)[512 + static_cast<size_t>(c)];
        at.tile[static_cast<size_t>(c)] = t < 192 && t / 48 < static_cast<int>(atlases.size());
    }
    // A code's pixel (u along x, v along z): the atlas is stored V-flipped (formats/terrain.md).
    auto rgb = [&](int code, int u, int v, int ch) {
        const int t = (*cht)[512 + static_cast<size_t>(code)];
        const Blob& a = *atlases[static_cast<size_t>(t / 48)];
        const int st = t % 48, tx = (st & 7) * kTile, ty = (st >> 3) * kTile;
        const uint8_t idx = a[static_cast<size_t>(ty + kTile - 1 - v) * 256 + tx + u];
        return (*col)[idx * 3u + static_cast<unsigned>(ch)] * 4;
    };

    // 1. PURE codes: they come back 1 to 4 cells further, across AND along (an edge tile only
    //    repeats along its edge; a 2x2 or 3x4 motif never touches itself).
    std::array<int, 256> h{}, v{};
    for (int d = 1; d <= 4; ++d) {
        std::array<int, 256> hd{}, vd{};
        for (int z = 0; z < kMap; ++z)
            for (int x = 0; x < kMap; ++x) {
                const uint8_t c = map[ck(x, z)];
                if (x + d < kMap && map[ck(x + d, z)] == c) ++hd[c];
                if (z + d < kMap && map[ck(x, z + d)] == c) ++vd[c];
            }
        for (int c = 0; c < 256; ++c) {
            h[static_cast<size_t>(c)] = std::max(h[static_cast<size_t>(c)], hd[static_cast<size_t>(c)]);
            v[static_cast<size_t>(c)] = std::max(v[static_cast<size_t>(c)], vd[static_cast<size_t>(c)]);
        }
    }
    std::array<std::array<int, 3>, 256> mean{};
    for (int c = 0; c < 256; ++c) {
        if (!at.tile[static_cast<size_t>(c)] || !at.count[static_cast<size_t>(c)]) continue;
        for (int ch = 0; ch < 3; ++ch) {
            int s = 0;
            for (int pv = 0; pv < kTile; ++pv)
                for (int pu = 0; pu < kTile; ++pu) s += rgb(c, pu, pv, ch);
            mean[static_cast<size_t>(c)][static_cast<size_t>(ch)] = s / (kTile * kTile);
        }
    }
    std::array<bool, 256> is_pure{};
    for (int c = 0; c < 256; ++c) {
        const int n = at.count[static_cast<size_t>(c)];
        is_pure[static_cast<size_t>(c)] = at.tile[static_cast<size_t>(c)] && n >= 10 && h[static_cast<size_t>(c)] * 100 > 15 * n &&
                                          v[static_cast<size_t>(c)] * 100 > 15 * n;
    }
    //    Pure codes of one ground lie directly side by side (0.3 to 2 times the rarer one's count:
    //    DIRT2's turned dirt, a motif's pieces), two grounds almost never (under 0.02) - and they
    //    look alike: a motif's pieces differ by up to 38, while CHOPPER2's water sits right
    //    against its rock with no shore tile.
    std::vector<int> adj(256 * 256, 0);
    for (int z = 0; z < kMap; ++z)
        for (int x = 0; x < kMap; ++x) {
            const uint8_t c = map[ck(x, z)];
            if (!is_pure[c]) continue;
            if (x + 1 < kMap && map[ck(x + 1, z)] != c && is_pure[map[ck(x + 1, z)]])
                ++adj[static_cast<size_t>(std::min(c, map[ck(x + 1, z)])) * 256 + std::max(c, map[ck(x + 1, z)])];
            if (z + 1 < kMap && map[ck(x, z + 1)] != c && is_pure[map[ck(x, z + 1)]])
                ++adj[static_cast<size_t>(std::min(c, map[ck(x, z + 1)])) * 256 + std::max(c, map[ck(x, z + 1)])];
        }
    std::array<int, 256> par;
    for (int c = 0; c < 256; ++c) par[static_cast<size_t>(c)] = c;
    auto root = [&](int c) {
        while (par[static_cast<size_t>(c)] != c) c = par[static_cast<size_t>(c)];
        return c;
    };
    for (int a = 0; a < 256; ++a)
        for (int b = a + 1; b < 256; ++b) {
            if (!is_pure[static_cast<size_t>(a)] || !is_pure[static_cast<size_t>(b)]) continue;
            const int n = adj[static_cast<size_t>(a) * 256 + b];
            int d = 0;
            for (int ch = 0; ch < 3; ++ch) d = std::max(d, std::abs(mean[static_cast<size_t>(a)][static_cast<size_t>(ch)] - mean[static_cast<size_t>(b)][static_cast<size_t>(ch)]));
            // The same pixels turned have the same mean: one ground, side by side or not. DIRT3
            // lays its dirt with tile 1 and turned ones here and there, seldom touching each
            // other: its road came out as four grounds of one colour, the track painted with a
            // quarter of it (2026-10-04).
            if (d <= 3 || (n && n * 4 >= std::min(at.count[static_cast<size_t>(a)], at.count[static_cast<size_t>(b)]) && d < 45))
                par[static_cast<size_t>(root(a))] = root(b);
        }
    std::vector<int> pure;
    for (int c = 0; c < 256; ++c)
        if (is_pure[static_cast<size_t>(c)]) pure.push_back(c);
    std::stable_sort(pure.begin(), pure.end(), [&](int a, int b) { return at.count[static_cast<size_t>(a)] > at.count[static_cast<size_t>(b)]; });
    std::vector<std::vector<int>> groups;
    {
        std::array<int, 256> slot;
        slot.fill(-1);
        for (int c : pure) {
            const int r = root(c);
            if (slot[static_cast<size_t>(r)] < 0) {
                slot[static_cast<size_t>(r)] = static_cast<int>(groups.size());
                groups.emplace_back();
            }
            groups[static_cast<size_t>(slot[static_cast<size_t>(r)])].push_back(c);
        }
        auto total = [&](const std::vector<int>& g) {
            int64_t s = 0;
            for (int c : g) s += at.count[static_cast<size_t>(c)];
            return s;
        };
        std::stable_sort(groups.begin(), groups.end(), [&](const auto& a, const auto& b) { return total(a) > total(b); });
        if (static_cast<int>(groups.size()) > kMaxM) groups.resize(kMaxM);
    }
    if (groups.empty()) return at;
    at.materials = static_cast<int>(groups.size());
    std::array<int, 256> seed;
    seed.fill(-1);
    for (int m = 0; m < at.materials; ++m) {
        const auto& g = groups[static_cast<size_t>(m)];
        for (int ch = 0; ch < 3; ++ch) {
            int s = 0;
            for (int c : g) s += mean[static_cast<size_t>(c)][static_cast<size_t>(ch)];
            at.colour[static_cast<size_t>(m)][static_cast<size_t>(ch)] = s / static_cast<int>(g.size());
        }
        for (int c : g) {
            seed[static_cast<size_t>(c)] = m;
            at.plain_of[static_cast<size_t>(c)] = static_cast<int8_t>(m);
        }
        at.fill[static_cast<size_t>(m)] = static_cast<uint8_t>(g[0]);
    }
    // THE WATER: FLAT (three quarters of its plain cells without a step), more blue than red (WATER1s is grey-green), the
    // LOWEST such ground - with any other within 16 height bytes of it (WATER3's river has two
    // shades, WATER1 two at sea level). By colour alone, a purple patch passed for WATER3's water.
    if (const Blob* mph = find_member(level, "MAP.MPH"); mph && mph->size() == map.size()) {
        auto step = [&](int x, int z) {
            const int hh = (*mph)[ck(x, z)];
            int s = 0;
            if (x + 1 < kMap) s = std::max(s, std::abs(hh - (*mph)[ck(x + 1, z)]));
            if (x > 0) s = std::max(s, std::abs(hh - (*mph)[ck(x - 1, z)]));
            if (z + 1 < kMap) s = std::max(s, std::abs(hh - (*mph)[ck(x, z + 1)]));
            if (z > 0) s = std::max(s, std::abs(hh - (*mph)[ck(x, z - 1)]));
            return s;
        };
        std::array<int, kMaxM> low;
        low.fill(-1);
        int lowest = -1;
        for (int m = 0; m < at.materials; ++m) {
            const auto& c3 = at.colour[static_cast<size_t>(m)];
            if (c3[2] <= c3[0]) continue;
            std::vector<int> hs, ss;
            for (int z = 0; z < kMap; ++z)
                for (int x = 0; x < kMap; ++x)
                    if (at.plain_of[map[ck(x, z)]] == m) {
                        hs.push_back((*mph)[ck(x, z)]);
                        ss.push_back(step(x, z));
                    }
            if (hs.size() < 50) continue;
            std::sort(hs.begin(), hs.end());
            std::sort(ss.begin(), ss.end());
            if (ss[ss.size() * 3 / 4] > 1) continue;
            low[static_cast<size_t>(m)] = hs[hs.size() / 2];
            if (lowest < 0 || low[static_cast<size_t>(m)] < low[static_cast<size_t>(lowest)]) lowest = m;
        }
        at.water = lowest;
        for (int m = 0; m < at.materials && lowest >= 0; ++m)
            at.wet[static_cast<size_t>(m)] = low[static_cast<size_t>(m)] >= 0 && low[static_cast<size_t>(m)] <= low[static_cast<size_t>(lowest)] + 16;
    }

    // 2. Every other code's corners, WITHOUT colour: the corners that meet at a vertex are one
    //    material, so each takes the vote of those it meets (weighted by how often), seeded by the
    //    pure codes, until nothing moves.
    constexpr int kSlots = 256 * 4;
    std::vector<int> pair(static_cast<size_t>(kSlots) * kSlots, 0);
    for (int z = 1; z < kMap; ++z)
        for (int x = 1; x < kMap; ++x) {
            int s[4];
            for (int i = 0; i < 4; ++i) s[i] = map[ck(x + kAround[i].dx, z + kAround[i].dz)] * 4 + kAround[i].corner;
            for (int i = 0; i < 4; ++i)
                for (int j = i + 1; j < 4; ++j)
                    if (s[i] != s[j]) {
                        ++pair[static_cast<size_t>(s[i]) * kSlots + s[j]];
                        ++pair[static_cast<size_t>(s[j]) * kSlots + s[i]];
                    }
        }
    std::vector<std::vector<std::pair<int, int>>> nb(kSlots);
    for (int a = 0; a < kSlots; ++a)
        for (int b = 0; b < kSlots; ++b)
            if (const int n = pair[static_cast<size_t>(a) * kSlots + b]) nb[static_cast<size_t>(a)].push_back({b, n});
    pair.clear();
    pair.shrink_to_fit();
    std::vector<int> lab(kSlots, -1);
    for (int c = 0; c < 256; ++c)
        if (seed[static_cast<size_t>(c)] >= 0)
            for (int i = 0; i < 4; ++i) lab[static_cast<size_t>(c) * 4 + i] = seed[static_cast<size_t>(c)];
    for (int it = 0; it < 50; ++it) {
        std::vector<int> next = lab;
        int moved = 0;
        for (int s = 0; s < kSlots; ++s) {
            if (seed[static_cast<size_t>(s / 4)] >= 0 || !at.count[static_cast<size_t>(s / 4)]) continue;
            int64_t vote[kMaxM] = {};
            bool any = false;
            for (const auto& [o, n] : nb[static_cast<size_t>(s)])
                if (lab[static_cast<size_t>(o)] >= 0) { vote[lab[static_cast<size_t>(o)]] += n; any = true; }
            if (!any) continue;
            int best = 0;
            for (int m = 1; m < at.materials; ++m)
                if (vote[m] > vote[best]) best = m;
            if (lab[static_cast<size_t>(s)] != best) { next[static_cast<size_t>(s)] = best; ++moved; }
        }
        lab.swap(next);
        if (!moved) break;
    }
    for (int c = 0; c < 256; ++c)
        for (int i = 0; i < 4; ++i) at.corners[static_cast<size_t>(c)][static_cast<size_t>(i)] = static_cast<int8_t>(lab[static_cast<size_t>(c) * 4 + i]);

    //    Which grounds meet: a tile of exactly those two, used 10 times or more.
    {
        std::array<std::array<int, kMaxM>, kMaxM> cells{};
        for (int c = 0; c < 256; ++c) {
            const auto& q = at.corners[static_cast<size_t>(c)];
            if (!at.tile[static_cast<size_t>(c)] || q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) continue;
            int a = q[0], b = q[0];
            bool two = true;
            for (int8_t m : q) {
                if (m == a || m == b) continue;
                if (a == b) b = m;
                else two = false;
            }
            if (two && a != b) {
                cells[static_cast<size_t>(a)][static_cast<size_t>(b)] += at.count[static_cast<size_t>(c)];
                cells[static_cast<size_t>(b)][static_cast<size_t>(a)] += at.count[static_cast<size_t>(c)];
            }
        }
        for (int a = 0; a < at.materials; ++a)
            for (int b = 0; b < at.materials; ++b)
                at.touch[static_cast<size_t>(a)][static_cast<size_t>(b)] = a == b || cells[static_cast<size_t>(a)][static_cast<size_t>(b)] >= 10;
    }

    // 3. Each code's material picture, kPic x kPic: every pixel (box-blurred 5x5) goes to the
    //    nearest colour among the materials of the code's corners, then a majority per block.
    for (int c = 0; c < 256; ++c) {
        if (!at.tile[static_cast<size_t>(c)] || !at.count[static_cast<size_t>(c)]) continue;
        int ms[4], nm = 0;
        for (int8_t m : at.corners[static_cast<size_t>(c)])
            if (m >= 0 && std::find(ms, ms + nm, m) == ms + nm) ms[nm++] = m;
        if (!nm) continue;
        std::sort(ms, ms + nm);
        int px[kTile][kTile][3];
        for (int pv = 0; pv < kTile; ++pv)
            for (int pu = 0; pu < kTile; ++pu)
                for (int ch = 0; ch < 3; ++ch) px[pv][pu][ch] = rgb(c, pu, pv, ch);
        uint8_t mat[kTile][kTile];
        for (int pv = 0; pv < kTile; ++pv)
            for (int pu = 0; pu < kTile; ++pu) {
                int s[3] = {0, 0, 0};
                for (int dv = -2; dv <= 2; ++dv)
                    for (int du = -2; du <= 2; ++du)
                        for (int ch = 0; ch < 3; ++ch) s[ch] += px[std::clamp(pv + dv, 0, kTile - 1)][std::clamp(pu + du, 0, kTile - 1)][ch];
                int64_t bd = -1;
                for (int i = 0; i < nm; ++i) {
                    int64_t d = 0;
                    for (int ch = 0; ch < 3; ++ch) {
                        const int64_t e = s[ch] - 25 * at.colour[static_cast<size_t>(ms[i])][static_cast<size_t>(ch)];
                        d += e * e;
                    }
                    if (bd < 0 || d < bd) { bd = d; mat[pv][pu] = static_cast<uint8_t>(ms[i]); }
                }
            }
        constexpr int kB = kTile / kPic;
        for (int i = 0; i < kPic; ++i)
            for (int j = 0; j < kPic; ++j) {
                int n[kMaxM] = {};
                for (int a = 0; a < kB; ++a)
                    for (int b = 0; b < kB; ++b) ++n[mat[i * kB + a][j * kB + b]];
                int best = 0;
                for (int m = 1; m < kMaxM; ++m)
                    if (n[m] > n[best]) best = m;
                at.picture[static_cast<size_t>(c)][static_cast<size_t>(i * kPic + j)] = static_cast<uint8_t>(best);
            }
    }

    // 4. What needs the vertex map: which corners sit on the water's ring, and each plain code's
    //    direction - the mean (doubled angles) of the strips it was laid in, away from the water.
    const std::vector<int8_t> V = vertex_materials(at, map);
    const std::vector<uint8_t> R = ring_of(at, V);
    std::array<std::array<int, 4>, 256> on_ring{};
    for (int z = 0; z < kMap; ++z)
        for (int x = 0; x < kMap; ++x) {
            const uint8_t c = map[ck(x, z)];
            for (int i = 0; i < 4; ++i) on_ring[c][static_cast<size_t>(i)] += R[vk(x + kCornerDx[i], z + kCornerDz[i])];
        }
    for (int c = 0; c < 256; ++c)
        for (int i = 0; i < 4; ++i) at.ring[static_cast<size_t>(c)][static_cast<size_t>(i)] = on_ring[static_cast<size_t>(c)][static_cast<size_t>(i)] * 2 > at.count[static_cast<size_t>(c)];
    const std::vector<uint8_t> near = near_water(at, V);
    static constexpr int kCos[8] = {1000, 707, 0, -707, -1000, -707, 0, 707};
    static constexpr int kSin[8] = {0, 707, 1000, 707, 0, -707, -1000, -707};
    for (int m = 0; m < at.materials; ++m) {
        const auto& g = groups[static_cast<size_t>(m)];
        if (g.size() < 2) continue;
        const std::vector<int8_t> st = strip_steps(V, m);
        for (int c : g) {
            int64_t sx = 0, sy = 0, n = 0;
            for (size_t k = 0; k < map.size(); ++k)
                if (map[k] == c && !near[k] && st[k] >= 0) {
                    sx += kCos[st[k]];
                    sy += kSin[st[k]];
                    ++n;
                }
            // concentrated: the mean vector longer than half
            if (n >= 3 && (sx * sx + sy * sy) * 4 > n * n * 1000000) at.angle[static_cast<size_t>(c)] = static_cast<int8_t>(octant(sx, sy));
        }
        // a level orients its plain tiles only if the most used one follows the strip itself
        // (DIRT2); DIRT3 lays its tile 1 everywhere and an oriented one here and there
        at.oriented[static_cast<size_t>(m)] = at.angle[at.fill[static_cast<size_t>(m)]] >= 0;
    }
    //    MOTIFS: classes of identical map rows and of identical map columns of the ground, and the
    //    code each (row class, column class) takes. Kept if it explains 60 % of the ground's cells
    //    and 10 points more than its most used code alone.
    for (int m = 0; m < at.materials; ++m) {
        const auto& g = groups[static_cast<size_t>(m)];
        if (g.size() < 2 || at.oriented[static_cast<size_t>(m)]) continue;
        auto plain = [&](int x, int z) { const uint8_t c = map[ck(x, z)]; return at.plain_of[c] == m ? static_cast<int>(c) : -1; };
        AutoTiles::Motif mo;
        mo.rows = line_classes([&](int line, int i) { return plain(i, line); }, mo.row);
        mo.cols = line_classes([&](int line, int i) { return plain(line, i); }, mo.col);
        std::vector<std::array<int, 256>> votes(static_cast<size_t>(mo.rows) * mo.cols);
        int cells = 0;
        for (int z = 0; z < kMap; ++z)
            for (int x = 0; x < kMap; ++x) {
                const int c = plain(x, z);
                if (c < 0) continue;
                ++cells;
                if (mo.row[static_cast<size_t>(z)] >= 0 && mo.col[static_cast<size_t>(x)] >= 0)
                    ++votes[static_cast<size_t>(mo.row[static_cast<size_t>(z)]) * mo.cols + mo.col[static_cast<size_t>(x)]][static_cast<size_t>(c)];
            }
        mo.code.assign(votes.size(), -1);
        int hit = 0;
        for (size_t i = 0; i < votes.size(); ++i) {
            int best = -1;
            for (int c = 0; c < 256; ++c)
                if (votes[i][static_cast<size_t>(c)] && (best < 0 || votes[i][static_cast<size_t>(c)] > votes[i][static_cast<size_t>(best)])) best = c;
            if (best >= 0) {
                mo.code[i] = static_cast<int16_t>(best);
                hit += votes[i][static_cast<size_t>(best)];
            }
        }
        const int fill_n = at.count[at.fill[static_cast<size_t>(m)]];
        if (cells && hit * 10 > cells * 6 && (hit - fill_n) * 10 > cells) {
            extend_classes(mo.row);
            extend_classes(mo.col);
            at.motif[static_cast<size_t>(m)] = std::move(mo);
        }
    }

    // 5. Where each material's boundary sits, as the painter drew it: the per-material offset that
    //    brings the most variant cells back right, one material at a time, twice over.
    Blob work = map;
    std::vector<VariantCell> cells;
    std::array<int, kMaxM> bias{};
    lay(at, bias, V, work, {}, &cells, false);
    Field field;
    field.build(V, at.materials);
    auto score = [&](const std::array<int, kMaxM>& b) {
        int ok = 0;
        for (const VariantCell& vc : cells) {
            const std::array<int8_t, 4> cr = {V[vk(vc.x, vc.z)], V[vk(vc.x + 1, vc.z)], V[vk(vc.x, vc.z + 1)], V[vk(vc.x + 1, vc.z + 1)]};
            ok += pick_variant(at, field, b, cr, vc.x, vc.z, vc.cand) == map[ck(vc.x, vc.z)];
        }
        return ok;
    };
    int best = score(bias);
    static constexpr int kSteps[] = {-kField * 60 / 100, -kField * 45 / 100, -kField * 30 / 100, -kField * 15 / 100,
                                     0, kField * 15 / 100, kField * 30 / 100};   // -0.6 .. 0.3
    for (int pass = 0; pass < 2; ++pass)
        for (int m = 0; m < at.materials; ++m)
            for (int s : kSteps) {
                std::array<int, kMaxM> b = bias;
                b[static_cast<size_t>(m)] = s;
                const int sc = score(b);
                if (sc > best) { best = sc; bias = b; }
            }
    at.bias = bias;

    // 6. The round trip: the level replayed from its vertex materials alone.
    work = map;
    lay(at, at.bias, V, work, {}, nullptr, false);
    int same = 0, equivalent = 0, cells_with_tile = 0;
    for (size_t k = 0; k < map.size(); ++k)
        if (at.tile[map[k]]) {
            ++cells_with_tile;
            same += work[k] == map[k];
            equivalent += work[k] == map[k] || (at.plain_of[work[k]] >= 0 && at.plain_of[work[k]] == at.plain_of[map[k]]);
        }
    at.equivalent_permille = cells_with_tile ? static_cast<int>(static_cast<int64_t>(equivalent) * 1000 / cells_with_tile) : 0;
    at.round_trip_permille = cells_with_tile ? static_cast<int>(static_cast<int64_t>(same) * 1000 / cells_with_tile) : 0;
    at.variant_permille = cells.empty() ? 1000 : static_cast<int>(static_cast<int64_t>(best) * 1000 / static_cast<int64_t>(cells.size()));
    char buf[256];
    std::snprintf(buf, sizeof buf, "autotuiles de %s : %d matieres, eau %d, aller-retour %d/%d cases (%d.%d %%, equivalentes %d.%d %%), variantes %d/%zu",
                  level_name.c_str(), at.materials, at.water, same, cells_with_tile, at.round_trip_permille / 10,
                  at.round_trip_permille % 10, at.equivalent_permille / 10, at.equivalent_permille % 10, best, cells.size());
    at.report = buf;
    return at;
}

} // namespace bigred::gen
