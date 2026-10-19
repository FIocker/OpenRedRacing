// The circuit generator. See the header.
//
// Coordinates are the game's own, as the files hold them: world units, X NOT mirrored (the
// mirror is the renderer's business), a texel every 512 units, texel (gx, gz) at (gx*512, gz*512).
// A bearing t points along (sin t, cos t) in (x, z) - X takes the SINE, as everywhere in sim/.
// Heights are kept in world units, UP positive, until they are written as MAP.MPH bytes (16 units
// a byte); a MAP.DAT Y is +Y DOWN, so the ground under a byte h is at Y = -16 h.

#include "gen/track_generator.h"

#include "gen/autotile.h"
#include "gen/ground_tiles.h"
#include "gen/hill_paint.h"
#include "gen/track_layers.h"

#include "formats/level_script.h"
#include "formats/scenery.h"
#include "sim/fixed.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace bigred::gen {

namespace {

constexpr int32_t kTexel = 512;          // world units per texel
constexpr int32_t kByte = 16;            // world units per height byte
constexpr int32_t kSampleStep = 256;     // the centre line, resampled
constexpr int kMap = 256;                // every generated map is 256 x 256, as the shipped ones

// --- a seeded integer generator -------------------------------------------------------------
// splitmix64: tiny, fast, and its output depends on nothing but the seed.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Uniform in [lo, hi], both included.
    int32_t range(int32_t lo, int32_t hi) {
        if (hi <= lo) return lo;
        const uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(hi) - lo) + 1;
        return static_cast<int32_t>(lo + static_cast<int64_t>(next() % span));
    }
};

// --- raw MAP.DAT records ----------------------------------------------------------------------
// Kept as the file's own fields rather than SceneryInstance, whose position is a float: what
// the generator writes must be exact.
struct DatRec {
    int8_t shape = 0;
    int32_t x = 0, y = 0, z = 0;
    int16_t ax = 0, ay = 0, az = 0;
    uint8_t variant = 0, category = 0, aux = 0;
};
constexpr size_t kDatRecord = 22;

int32_t rd32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}
int16_t rd16(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8)));
}
void wr32(Blob& b, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(u >> (8 * i)));
}
void wr16(Blob& b, int32_t v) {
    const uint16_t u = static_cast<uint16_t>(v);
    b.push_back(static_cast<uint8_t>(u));
    b.push_back(static_cast<uint8_t>(u >> 8));
}

std::vector<DatRec> read_dat(const Blob& b) {
    std::vector<DatRec> out;
    for (size_t o = 0; o + kDatRecord <= b.size(); o += kDatRecord) {
        const uint8_t* p = b.data() + o;
        DatRec r;
        r.shape = static_cast<int8_t>(p[0]);
        r.x = rd32(p + 1);
        r.y = rd32(p + 5);
        r.z = rd32(p + 9);
        r.ax = rd16(p + 13);
        r.ay = rd16(p + 15);
        r.az = rd16(p + 17);
        r.variant = p[19];
        r.category = p[20];
        r.aux = p[21];
        out.push_back(r);
    }
    return out;
}

Blob write_dat(const std::vector<DatRec>& recs) {
    Blob b;
    b.reserve(recs.size() * kDatRecord);
    for (const DatRec& r : recs) {
        b.push_back(static_cast<uint8_t>(r.shape));
        wr32(b, r.x);
        wr32(b, r.y);
        wr32(b, r.z);
        wr16(b, r.ax);
        wr16(b, r.ay);
        wr16(b, r.az);
        b.push_back(r.variant);
        b.push_back(r.category);
        b.push_back(r.aux);
    }
    return b;
}

Blob write_cse(const std::vector<Gate>& gates) {
    Blob b;
    for (const Gate& g : gates) {
        wr32(b, g.x);
        wr32(b, g.z);
        wr16(b, g.angle);
        wr16(b, g.length);
        wr16(b, g.half_depth);
        wr16(b, g.half_depth_delta);
    }
    return b;
}

// A gate's heading in BAM16 (its table index times 64), and the centre of its slab across the
// road: the anchor is on one verge, the far verge is `length` away along (sin, cos).
int32_t gate_bam(const Gate& g) { return (static_cast<int32_t>(g.angle) & 0x3FF) << 6; }
Point gate_centre(const Gate& g) {
    const int32_t a = gate_bam(g), half = g.length / 2;
    return {g.x + fx::mul15(half, fx::sin16(a)), g.z + fx::mul15(half, fx::cos16(a))};
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const Blob& member(const Archive& a, const std::string& name) {
    const Blob* b = find_member(a, name);
    if (!b) throw std::runtime_error("the donor has no " + name);
    return *b;
}

// The most common value of a byte histogram, skipping `skip`.
uint8_t mode_of(const std::array<int, 256>& hist, int skip = -1) {
    int best = -1, best_n = -1;
    for (int v = 0; v < 256; ++v)
        if (v != skip && hist[static_cast<size_t>(v)] > best_n) {
            best = v;
            best_n = hist[static_cast<size_t>(v)];
        }
    return static_cast<uint8_t>(best < 0 ? 0 : best);
}

int isqrt64(int64_t v) {
    if (v <= 0) return 0;
    int64_t r = 1;
    while (r * r <= v) r <<= 1;
    int64_t lo = r >> 1, hi = r;
    while (lo + 1 < hi) {
        const int64_t mid = (lo + hi) / 2;
        (mid * mid <= v ? lo : hi) = mid;
    }
    return static_cast<int>(lo);
}

// Smoothstep on 0..den -> 0..1024, integer.
int32_t smooth1024(int64_t t, int64_t den) {
    if (t <= 0) return 0;
    if (t >= den) return 1024;
    return static_cast<int32_t>((t * t * (3 * den - 2 * t) * 1024) / (den * den * den));
}

} // namespace

// ---------------------------------------------------------------------------------------------
const std::vector<Theme>& themes() {
    static const std::vector<Theme> t = {
        {"DIRT", "DIRT1", {"DIRT1", "DIRT2", "DIRT3"}},
        {"CITY", "CITY1", {"CITY1", "CITY2", "CITY3", "CITY4"}},
        {"SNOW", "SNOW1", {"SNOW1", "SNOW2", "SNOW3"}},
        {"RIG", "RIG1", {"RIG1", "RIG3", "RIG4"}},
        {"MOON", "MOON1", {"MOON1", "MOON2", "MOON3"}},
        {"WATER", "WATER1", {"WATER1", "WATER2", "WATER3"}},
        {"CHOPPER", "CHOPPER1", {"CHOPPER1", "CHOPPER2", "CHOPPER3"}},
    };
    return t;
}

const Theme* find_theme(const std::string& name) {
    const std::string u = upper(name);
    for (const Theme& t : themes())
        if (t.name == u) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
DonorInfo analyse_donor(const Archive& lev, const std::string& level_name) {
    DonorInfo d;
    const Blob& mph = member(lev, "MAP.MPH");
    const Blob& mpc = member(lev, "MAP.MPC");
    const int w = isqrt64(static_cast<int64_t>(mph.size()));
    const int h = w;
    std::vector<Gate> gates;
    if (const Blob* cse = find_member(lev, "MAP.CSE")) gates = parse_gates(cse->data(), cse->size());
    std::string txt;
    if (const Blob* t = find_member(lev, level_name + ".TXT")) txt.assign(t->begin(), t->end());
    auto mpc_at = [&](int32_t x, int32_t z) -> int {
        const int gx = x / kTexel, gz = z / kTexel;
        if (gx < 0 || gz < 0 || gx >= w || gz >= h) return -1;
        return mpc[static_cast<size_t>(gz) * w + gx];
    };

    // The ground codes. ROAD: under each gate's centre. Its HALF WIDTH: how far the road code
    // goes on each side of it. VERGE: just past that. FIELD: far from every gate.
    std::array<int, 256> road{}, verge{}, field{};
    std::vector<int> widths;
    for (const Gate& g : gates) {
        const Point c = gate_centre(g);
        const int v = mpc_at(c.x, c.z);
        if (v >= 0) ++road[static_cast<size_t>(v)];
    }
    d.road_mpc = mode_of(road);
    for (const Gate& g : gates) {
        const Point c = gate_centre(g);
        if (mpc_at(c.x, c.z) != d.road_mpc) continue;
        const int32_t a = gate_bam(g), s = fx::sin16(a), co = fx::cos16(a);
        for (int side = -1; side <= 1; side += 2) {
            int32_t e = 0;
            while (e < 6000 && mpc_at(c.x + side * fx::mul15(e, s), c.z + side * fx::mul15(e, co)) ==
                                   d.road_mpc)
                e += 64;
            widths.push_back(e);
            for (int32_t k = e + 128; k < e + 900; k += 128) {
                const int v = mpc_at(c.x + side * fx::mul15(k, s), c.z + side * fx::mul15(k, co));
                if (v >= 0) ++verge[static_cast<size_t>(v)];
            }
        }
    }
    if (!widths.empty()) {
        std::sort(widths.begin(), widths.end());
        d.road_half_width = std::clamp(widths[widths.size() / 2], 1000, 1400);
    }
    d.verge_mpc = mode_of(verge, d.road_mpc);
    // The road's centre line, approximately: the gate centres, joined every 512 units.
    std::vector<Point> centres;
    for (size_t g = 0; g < gates.size(); ++g) {
        const Point a = gate_centre(gates[g]), b = gate_centre(gates[(g + 1) % gates.size()]);
        const int32_t len = fx::hypot32(b.x - a.x, b.z - a.z);
        const int steps = std::max(1, len / 512);
        for (int s = 0; s < steps; ++s)
            centres.push_back({a.x + (b.x - a.x) * s / steps, a.z + (b.z - a.z) * s / steps});
    }
    std::array<int, 256> heights{}, road_heights{};
    std::array<int, 256> land{};
    std::vector<int32_t> road_dist(static_cast<size_t>(w) * h, 0);   // world units
    std::vector<std::vector<int>> xsec(DonorInfo::kXsecBins);
    std::vector<std::array<int, 256>> bands(DonorInfo::kBandBins, std::array<int, 256>{});
    for (int gz = 0; gz < h; ++gz)
        for (int gx = 0; gx < w; ++gx) {
            const size_t i = static_cast<size_t>(gz) * w + gx;
            ++heights[mph[i]];
            int64_t best = INT64_MAX;
            size_t best_c = 0;
            for (size_t ci = 0; ci < centres.size(); ++ci) {
                const Point& c = centres[ci];
                const int64_t dx = gx * kTexel - c.x, dz = gz * kTexel - c.z;
                if (dx * dx + dz * dz < best) {
                    best = dx * dx + dz * dz;
                    best_c = ci;
                }
            }
            road_dist[i] = isqrt64(best);
            if (!centres.empty()) {
                const Point& c = centres[best_c];
                const int cgx = std::clamp(c.x / kTexel, 0, w - 1), cgz = std::clamp(c.z / kTexel, 0, h - 1);
                const int delta = static_cast<int>(mph[i]) - static_cast<int>(mph[static_cast<size_t>(cgz) * w + cgx]);
                const int bin = road_dist[i] / DonorInfo::kXsecStep;
                if (bin < DonorInfo::kXsecBins) xsec[static_cast<size_t>(bin)].push_back(delta);
                const int band = road_dist[i] / DonorInfo::kBandStep;
                if (band < DonorInfo::kBandBins) ++bands[static_cast<size_t>(band)][mpc[i]];
            }
            if (road_dist[i] < 600) ++road_heights[mph[i]];
            if (mpc[i] == 240) continue;
            if (road_dist[i] > 16000) ++field[mpc[i]];
            else if (road_dist[i] > d.road_half_width + 1500 && road_dist[i] < 6000) ++land[mpc[i]];
        }
    // OUTSIDE: what fills the map far from the road - the sea round DIRT1's island. LAND: what
    // surrounds the road itself.
    d.field_mpc = mode_of(field);
    for (int b = 0; b < DonorInfo::kXsecBins; ++b) {
        std::vector<int>& v = xsec[static_cast<size_t>(b)];
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        const int med = v[v.size() / 2];
        std::vector<int> dev;
        for (int x : v) dev.push_back(std::abs(x - med));
        std::sort(dev.begin(), dev.end());
        d.xsec_median[b] = med;
        d.xsec_spread[b] = dev[dev.size() / 2];
    }
    for (int b = 0; b < DonorInfo::kBandBins; ++b) d.band_mpc[b] = mode_of(bands[static_cast<size_t>(b)]);
    for (size_t g = 0; g < gates.size(); ++g) {
        const Point a = gate_centre(gates[g]), b = gate_centre(gates[(g + 1) % gates.size()]);
        d.lap_length += fx::hypot32(b.x - a.x, b.z - a.z);
    }
    // When the verge IS the outside, the road runs straight on the plain (RIG3, CITY3, WATER1):
    // that plain is the land. Leaving it out made RIG3's land its canal - code 0, water.
    if (d.verge_mpc == d.field_mpc) {
        d.land_mpc = d.verge_mpc;
    } else {
        std::array<int, 256> l = land;
        l[d.road_mpc] = 0;
        l[d.verge_mpc] = 0;
        d.land_mpc = mode_of(l);
        if (l[d.land_mpc] == 0) d.land_mpc = d.verge_mpc;
    }
    {
        int acc = 0, total = 0;
        for (int v : road_heights) total += v;
        for (int v = 0; v < 256; ++v) {
            if (acc < (total + 1) / 2 && acc + road_heights[static_cast<size_t>(v)] >= (total + 1) / 2) d.road_height = v;
            acc += road_heights[static_cast<size_t>(v)];
        }
    }
    // THE COAST: the distance from the road past which most of the ground is the outside code,
    // in 1 000-unit bands. Zero when the outside is the land itself (MOON: one surface).
    if (d.field_mpc != d.land_mpc && d.field_mpc != d.road_mpc) {
        for (int band = 2; band < 40; ++band) {
            int n = 0, out = 0;
            for (size_t i = 0; i < road_dist.size(); ++i)
                if (road_dist[i] / 1000 == band) {
                    ++n;
                    out += (mpc[i] == d.field_mpc) ? 1 : 0;
                }
            if (n > 0 && out * 2 > n) {
                d.coast_distance = band * 1000;
                break;
            }
        }
        std::array<int, 256> oh{};
        for (size_t i = 0; i < road_dist.size(); ++i)
            if (mpc[i] == d.field_mpc) ++oh[mph[i]];
        d.outside_height = mode_of(oh);
    }

    // THE MASSIF: the plain's level, how much of the land is raised at each distance from the
    // road, and how a hill rises from its foot. The land: every texel but the outside and holes.
    {
        const bool island = d.coast_distance > 0;
        // ⚠️ Not every texel is terrain: WATER2's rows 0-55, off the playable area, are random
        // bytes. A texel whose 5x5 neighbourhood has a median step over 40 is left out (a real
        // cliff is a line: the median round it stays low).
        std::vector<uint8_t> noise(mph.size(), 0);
        {
            std::vector<int> st(mph.size(), 0);
            for (int gz = 0; gz < h; ++gz)
                for (int gx = 0; gx < w; ++gx) {
                    const size_t i = static_cast<size_t>(gz) * w + gx;
                    if (gx + 1 < w) st[i] = std::max(st[i], std::abs(mph[i] - mph[i + 1]));
                    if (gz + 1 < h) st[i] = std::max(st[i], std::abs(mph[i] - mph[i + static_cast<size_t>(w)]));
                }
            std::vector<int> win;
            for (int gz = 0; gz < h; ++gz)
                for (int gx = 0; gx < w; ++gx) {
                    win.clear();
                    for (int z = std::max(0, gz - 2); z <= std::min(h - 1, gz + 2); ++z)
                        for (int x = std::max(0, gx - 2); x <= std::min(w - 1, gx + 2); ++x) win.push_back(st[static_cast<size_t>(z) * w + x]);
                    std::nth_element(win.begin(), win.begin() + static_cast<long>(win.size() / 2), win.end());
                    noise[static_cast<size_t>(gz) * w + gx] = win[win.size() / 2] > 40 ? 1 : 0;
                }
        }
        auto is_land = [&](size_t i) { return !noise[i] && mpc[i] != 240 && !(island && mpc[i] == d.field_mpc); };
        std::array<int, 256> plain{};
        for (size_t i = 0; i < mph.size(); ++i)
            if (is_land(i) && road_dist[i] > 3000) ++plain[mph[i]];
        d.base_height = mode_of(plain);
        std::vector<uint8_t> raised(mph.size(), 0);
        std::array<int, DonorInfo::kMassifBins> n{}, up{};
        for (size_t i = 0; i < mph.size(); ++i) {
            if (!is_land(i)) continue;
            raised[i] = mph[i] > d.base_height + 2 ? 1 : 0;
            d.max_excess = std::max(d.max_excess, mph[i] - d.base_height);
            const int b = road_dist[i] / DonorInfo::kMassifStep;
            if (b >= DonorInfo::kMassifBins) continue;
            ++n[static_cast<size_t>(b)];
            up[static_cast<size_t>(b)] += raised[i];
        }
        for (int b = 0; b < DonorInfo::kMassifBins; ++b)
            d.raised_permille[b] = n[static_cast<size_t>(b)] ? up[static_cast<size_t>(b)] * 1000 / n[static_cast<size_t>(b)] : 0;
        // The distance from the foot, a chamfer (3 a side, 4 a diagonal) in thirds of a texel.
        std::vector<int32_t> foot(mph.size(), 0);
        for (size_t i = 0; i < mph.size(); ++i) foot[i] = raised[i] ? INT32_MAX / 2 : 0;
        for (int pass = 0; pass < 2; ++pass) {
            const int step = pass == 0 ? 1 : -1;
            for (int gz = pass == 0 ? 0 : h - 1; gz >= 0 && gz < h; gz += step)
                for (int gx = pass == 0 ? 0 : w - 1; gx >= 0 && gx < w; gx += step) {
                    int32_t& c = foot[static_cast<size_t>(gz) * w + gx];
                    auto at = [&](int x, int z) { return (x < 0 || z < 0 || x >= w || z >= h) ? 0 : foot[static_cast<size_t>(z) * w + x]; };
                    c = std::min({c, at(gx - step, gz) + 3, at(gx, gz - step) + 3, at(gx - step, gz - step) + 4,
                                  at(gx + step, gz - step) + 4});
                }
        }
        std::vector<std::vector<int>> rise(DonorInfo::kFootBins);
        for (size_t i = 0; i < mph.size(); ++i) {
            if (!raised[i]) continue;
            const int k = (foot[i] + 1) / 3;
            if (k < DonorInfo::kFootBins) rise[static_cast<size_t>(k)].push_back(mph[i] - d.base_height);
        }
        for (int k = 1; k < DonorInfo::kFootBins; ++k) {
            std::vector<int>& v = rise[static_cast<size_t>(k)];
            if (v.size() < 8) {
                d.foot_rise[k] = d.foot_rise[k - 1];   // no hill that wide: a plateau
                continue;
            }
            std::sort(v.begin(), v.end());
            d.foot_rise[k] = std::max(d.foot_rise[k - 1], v[v.size() / 2]);
        }
    }

    // The relief: median, range, and the edge profile.
    {
        int acc = 0, total = w * h;
        d.min_height = 255;
        d.max_height = 0;
        for (int v = 0; v < 256; ++v) {
            if (heights[static_cast<size_t>(v)] == 0) continue;
            d.min_height = std::min(d.min_height, v);
            d.max_height = std::max(d.max_height, v);
            if (acc < total / 2 && acc + heights[static_cast<size_t>(v)] >= total / 2) d.median_height = v;
            acc += heights[static_cast<size_t>(v)];
        }
        for (int ring = 0; ring < DonorInfo::kEdgeRings; ++ring) {
            int64_t sum = 0;
            int n = 0;
            for (int gz = 0; gz < h; ++gz)
                for (int gx = 0; gx < w; ++gx) {
                    const int e = std::min(std::min(gx, gz), std::min(w - 1 - gx, h - 1 - gz));
                    if (e != ring) continue;
                    sum += mph[static_cast<size_t>(gz) * w + gx];
                    ++n;
                }
            d.edge_mean[ring] = n ? static_cast<int>(sum / n) : 0;
        }
    }

    // CHECKP: which slot, which category, and its yaw against the gate it lies in.
    std::vector<std::string> shl;
    if (const Blob* s = find_member(lev, "MAP.SHL")) shl = parse_map_shl(s->data(), s->size());
    for (size_t i = 0; i < shl.size(); ++i)
        if (upper(shl[i]) == "CHECKP") d.checkp_slot = static_cast<int>(i);
    std::vector<DatRec> recs;
    if (const Blob* dat = find_member(lev, "MAP.DAT")) recs = read_dat(*dat);
    std::map<int32_t, int> same, opposite;   // offsets in 0x400 steps: yaw - gate, yaw + gate
    std::vector<int32_t> lifts;
    const Blob* lift_mph = find_member(lev, "MAP.MPH");
    for (const DatRec& r : recs) {
        if (r.shape != d.checkp_slot || gates.empty()) continue;
        d.checkp_category = r.category;
        if (lift_mph && lift_mph->size() == static_cast<size_t>(kMap) * kMap) {
            const int gx = std::clamp(r.x / kTexel, 0, kMap - 1), gz = std::clamp(r.z / kTexel, 0, kMap - 1);
            lifts.push_back(-r.y - static_cast<int32_t>((*lift_mph)[static_cast<size_t>(gz) * kMap + gx]) * kByte);
        }
        size_t best = 0;
        int64_t best_d = INT64_MAX;
        for (size_t g = 0; g < gates.size(); ++g) {
            const Point c = gate_centre(gates[g]);
            const int64_t dx = r.x - c.x, dz = r.z - c.z;
            if (dx * dx + dz * dz < best_d) {
                best_d = dx * dx + dz * dz;
                best = g;
            }
        }
        const int32_t ga = gate_bam(gates[best]);
        ++same[(((static_cast<int32_t>(r.ay) - ga) & 0xFFFF) + 0x200) / 0x400 % 64];
        ++opposite[(((static_cast<int32_t>(r.ay) + ga) & 0xFFFF) + 0x200) / 0x400 % 64];
    }
    auto top = [](const std::map<int32_t, int>& m, int32_t& key) {
        int n = 0;
        for (const auto& [k, v] : m)
            if (v > n) {
                n = v;
                key = k;
            }
        return n;
    };
    int32_t same_key = 0, opp_key = 0;
    const int same_n = top(same, same_key), opp_n = top(opposite, opp_key);
    d.checkp_yaw_offset = (same_key * 0x400) & 0xFFFF;
    if (!lifts.empty()) {
        std::sort(lifts.begin(), lifts.end());
        d.checkp_lift = std::max<int32_t>(0, lifts[lifts.size() / 2]);
    }

    // The grid, in gate 0's frame.
    const StartingGrid grid = parse_grid(txt);
    if (grid.present && !gates.empty()) {
        const Gate& g0 = gates[0];
        const Point c = gate_centre(g0);
        const int32_t a = gate_bam(g0);
        // Forward (x, z) = (cos a, -sin a); across (from the anchor to the far verge) = (sin a, cos a).
        const int64_t dx = grid.x - c.x, dz = grid.z - c.z;
        d.grid_along = static_cast<int32_t>((dx * fx::cos16(a) - dz * fx::sin16(a)) / 32768);
        d.grid_across = static_cast<int32_t>((dx * fx::sin16(a) + dz * fx::cos16(a)) / 32768);
        d.grid_yaw_offset = static_cast<int16_t>((grid.yaw_bam16 - a) & 0xFFFF);
    }

    char buf[512];
    std::string& r = d.report;
    std::snprintf(buf, sizeof buf, "%s : %dx%d, %zu portes, %zu instances\n", level_name.c_str(), w, h,
                  gates.size(), recs.size());
    r += buf;
    std::snprintf(buf, sizeof buf,
                  "  sol : route %d (demi-largeur %d), bas-cote %d, terre %d, dehors %d (cote a %d, "
                  "hauteur %d) ; route a la hauteur %d\n", d.road_mpc, d.road_half_width, d.verge_mpc,
                  d.land_mpc, d.field_mpc, d.coast_distance, d.outside_height, d.road_height);
    r += buf;
    std::snprintf(buf, sizeof buf, "  relief : min %d, mediane %d, max %d (octets) ; tour de %d unites\n",
                  d.min_height, d.median_height, d.max_height, d.lap_length);
    r += buf;
    std::snprintf(buf, sizeof buf, "  massif : plaine a %d, jusqu'a +%d ; sureleve (pour mille, par 2000 unites de la route) :",
                  d.base_height, d.max_excess);
    r += buf;
    for (int i = 0; i < DonorInfo::kMassifBins; ++i) {
        std::snprintf(buf, sizeof buf, " %d", d.raised_permille[i]);
        r += buf;
    }
    r += "\n  montee depuis le pied (octets, par case) :";
    for (int i = 1; i < DonorInfo::kFootBins; ++i) {
        std::snprintf(buf, sizeof buf, " %d", d.foot_rise[i]);
        r += buf;
    }
    r += "\n";
    r += "  coupe (octets sous/sur la route, par 500 unites ; ecart) :";
    for (int i = 0; i < DonorInfo::kXsecBins; i += 2) {
        std::snprintf(buf, sizeof buf, " %+d/%d", d.xsec_median[i], d.xsec_spread[i]);
        r += buf;
    }
    r += "\n  bandes de sol (par 256 unites) :";
    for (int i = 0; i < DonorInfo::kBandBins; ++i) {
        std::snprintf(buf, sizeof buf, " %d", d.band_mpc[i]);
        r += buf;
    }
    r += "\n";
    r += "  bords (moyenne par anneau depuis le bord) :";
    for (int i = 0; i < DonorInfo::kEdgeRings; ++i) {
        std::snprintf(buf, sizeof buf, " %d", d.edge_mean[i]);
        r += buf;
    }
    r += "\n";
    std::snprintf(buf, sizeof buf,
                  "  CHECKP : case %d, categorie %d, lacet - porte = 0x%04X (%d lignes sur %d), "
                  "lacet + porte = 0x%04X (%d), hauteur au-dessus du sol %d\n",
                  d.checkp_slot, d.checkp_category, same_key * 0x400, same_n,
                  static_cast<int>(std::count_if(recs.begin(), recs.end(),
                                                 [&](const DatRec& q) { return q.shape == d.checkp_slot; })),
                  opp_key * 0x400, opp_n, d.checkp_lift);
    r += buf;
    std::snprintf(buf, sizeof buf, "  grille : %d le long, %d en travers de la porte 0, cap %+d\n",
                  d.grid_along, d.grid_across, static_cast<int16_t>(d.grid_yaw_offset));
    r += buf;
    // The shapes, with their category and count - what the scatter can borrow.
    std::map<std::pair<int, int>, int> counts;
    for (const DatRec& q : recs) ++counts[{q.shape, q.category}];
    r += "  formes (case:nom/categorie x nombre) :";
    for (const auto& [k, n] : counts) {
        const std::string nm = (k.first >= 0 && static_cast<size_t>(k.first) < shl.size())
                                   ? shl[static_cast<size_t>(k.first)] : "?";
        std::snprintf(buf, sizeof buf, " %s/%d x%d", nm.c_str(), k.second, n);
        r += buf;
    }
    r += "\n";
    return d;
}

// ---------------------------------------------------------------------------------------------
std::vector<int32_t> road_profile(const Archive& lev) {
    std::vector<int32_t> out;
    const Blob* mph = find_member(lev, "MAP.MPH");
    const Blob* cse = find_member(lev, "MAP.CSE");
    if (!mph || !cse) return out;
    const int w = isqrt64(static_cast<int64_t>(mph->size()));
    const std::vector<Gate> gates = parse_gates(cse->data(), cse->size());
    // Bilinear on the height bytes, in world units.
    auto height = [&](int32_t x, int32_t z) {
        const int gx = std::clamp(x / kTexel, 0, w - 2), gz = std::clamp(z / kTexel, 0, w - 2);
        const int64_t fx_ = std::clamp(x - gx * kTexel, 0, kTexel), fz = std::clamp(z - gz * kTexel, 0, kTexel);
        auto H = [&](int cx, int cz) { return static_cast<int64_t>((*mph)[static_cast<size_t>(cz) * w + cx]) * kByte; };
        const int64_t top = H(gx, gz) * (kTexel - fx_) + H(gx + 1, gz) * fx_;
        const int64_t bot = H(gx, gz + 1) * (kTexel - fx_) + H(gx + 1, gz + 1) * fx_;
        return static_cast<int32_t>((top * (kTexel - fz) + bot * fz) / (int64_t(kTexel) * kTexel));
    };
    for (size_t g = 0; g < gates.size(); ++g) {
        const Point a = gate_centre(gates[g]), b = gate_centre(gates[(g + 1) % gates.size()]);
        const int32_t len = fx::hypot32(b.x - a.x, b.z - a.z);
        const int steps = std::max(1, len / kSampleStep);
        for (int s = 0; s < steps; ++s)
            out.push_back(height(a.x + (b.x - a.x) * s / steps, a.z + (b.z - a.z) * s / steps));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
namespace {

// The centre line: control points around the map's centre, a closed Catmull-Rom spline through
// them, resampled every kSampleStep units. Star-shaped around the centre by construction (one
// point per angular sector), so it cannot cross itself between the control points; the checks
// below catch what the spline's overshoot can still do.
bool draw_centre_line(Rng& rng, std::vector<Point>& out, std::string& why) {
    // The size first - the shipped laps run 68 000 to 270 000 units, most of them 110 000 to
    // 190 000 (--analyse) - then the centre, moved off the map's own so that the island is
    // not always the same shape round it.
    const int32_t room = (kMap / 2 - 34) * kTexel;
    const int32_t rmax = rng.range(room * 9 / 20, room * 3 / 4), rmin = rmax * 3 / 10;
    const int32_t slack = room - rmax;
    const int32_t cx = kMap * kTexel / 2 + rng.range(-slack, slack);
    const int32_t cz = kMap * kTexel / 2 + rng.range(-slack, slack);
    const int k = rng.range(6, 12);
    std::vector<Point> ctrl;
    for (int i = 0; i < k; ++i) {
        const int32_t step = 65536 / k;
        const int32_t a = i * step + rng.range(-step * 2 / 5, step * 2 / 5);
        const int32_t r = rng.range(rmin, rmax);
        ctrl.push_back({cx + fx::mul15(r, fx::sin16(a)), cz + fx::mul15(r, fx::cos16(a))});
    }
    // The spline, densely: 32 steps a segment.
    constexpr int64_t T = 32;
    std::vector<Point> dense;
    for (int i = 0; i < k; ++i) {
        const Point& p0 = ctrl[static_cast<size_t>((i + k - 1) % k)];
        const Point& p1 = ctrl[static_cast<size_t>(i)];
        const Point& p2 = ctrl[static_cast<size_t>((i + 1) % k)];
        const Point& p3 = ctrl[static_cast<size_t>((i + 2) % k)];
        for (int64_t t = 0; t < T; ++t) {
            auto cr = [&](int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
                const int64_t v = 2 * a1 * T * T * T + (-a0 + a2) * t * T * T +
                                  (2 * a0 - 5 * a1 + 4 * a2 - a3) * t * t * T +
                                  (-a0 + 3 * a1 - 3 * a2 + a3) * t * t * t;
                return static_cast<int32_t>(v / (2 * T * T * T));
            };
            dense.push_back({cr(p0.x, p1.x, p2.x, p3.x), cr(p0.z, p1.z, p2.z, p3.z)});
        }
    }
    // Resampled at a constant step along the curve.
    out.clear();
    out.push_back(dense[0]);
    int32_t carry = 0;   // distance already walked past the last sample
    for (size_t i = 0; i < dense.size(); ++i) {
        const Point a = dense[i], b = dense[(i + 1) % dense.size()];
        const int32_t len = fx::hypot32(b.x - a.x, b.z - a.z);
        int32_t at = kSampleStep - carry;
        while (at <= len) {
            out.push_back({a.x + static_cast<int32_t>(static_cast<int64_t>(b.x - a.x) * at / std::max(len, 1)),
                           a.z + static_cast<int32_t>(static_cast<int64_t>(b.z - a.z) * at / std::max(len, 1))});
            at += kSampleStep;
        }
        carry = len - (at - kSampleStep);
    }
    if (out.size() > 1) out.pop_back();   // the last one sits on the first
    const int n = static_cast<int>(out.size());

    // 1. Inside the map, with room for the edge.
    const int32_t lo = 24 * kTexel, hi = (kMap - 24) * kTexel;
    for (const Point& p : out)
        if (p.x < lo || p.z < lo || p.x > hi || p.z > hi) {
            why = "sort de la carte";
            return false;
        }
    // 2. No bend tighter than kMinRadius, measured over four samples (a kilo-unit of road).
    constexpr int32_t kMinRadius = 2000;
    const int32_t max_turn4 = static_cast<int32_t>(int64_t(4) * kSampleStep * 10430 / kMinRadius);
    std::vector<int32_t> heading(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Point& a = out[static_cast<size_t>(i)];
        const Point& b = out[static_cast<size_t>((i + 1) % n)];
        heading[static_cast<size_t>(i)] = fx::atan2_bam16(b.x - a.x, b.z - a.z);
    }
    for (int i = 0; i < n; ++i) {
        const int16_t d = static_cast<int16_t>(heading[static_cast<size_t>((i + 4) % n)] -
                                               heading[static_cast<size_t>(i)]);
        if (std::abs(static_cast<int32_t>(d)) > max_turn4) {
            why = "virage trop serre";
            return false;
        }
    }
    // 3. No two stretches of road closer than kMinSeparation unless they are neighbours along it.
    constexpr int32_t kMinSeparation = 9000;
    const int near_samples = 60;   // 15 000 units of road either way
    for (int i = 0; i < n; i += 2)
        for (int j = i + near_samples; j < n; j += 2) {
            if (n - (j - i) < near_samples) continue;
            const int64_t dx = out[static_cast<size_t>(i)].x - out[static_cast<size_t>(j)].x;
            const int64_t dz = out[static_cast<size_t>(i)].z - out[static_cast<size_t>(j)].z;
            if (dx * dx + dz * dz < int64_t(kMinSeparation) * kMinSeparation) {
                why = "deux troncons trop proches";
                return false;
            }
        }
    // 4. Neither a sprint nor a marathon.
    if (n * kSampleStep < 100000 || n * kSampleStep > 200000) {
        why = "longueur hors bornes";
        return false;
    }
    return true;
}

// THE CENTRE LINE AS BEZIER CURVES (the user's method, 2026-10-04): a polygon of STRAIGHTS whose
// corners are rounded by cubic Bezier curves, each corner with its own track width - the donor's,
// or widened or narrowed within the spread of its road.
//
// Every straight runs in a direction the atlas draws cleanly: along an axis, a 45-degree diagonal
// or a 2:1 slope, 16 in all (user, 2026-10-04: "les bords de la piste sont souvent en escalier").
// Measured on the shipped circuits: 29 to 34 % of DIRT2's, DIRT3's and SNOW1's road runs within
// 4 degrees of an axis (DIRT2 is a rounded rectangle); free curves had 4 to 11 %, and between the
// clean directions an edge can only be drawn in steps.
//
// The polygon: corners drawn round a centre as before, each side turned to the nearest clean
// direction (its length kept), the last two sides solved to close the loop in clean directions.
// The corners: tangent points at `round` before and after the corner along its two sides, a cubic
// from one to the other with its handles along the sides (a short `round`: a tight corner; a long
// one: a sweeping bend).
//
// Then what the shipped laps show (2026-10-04 review, 21 circuits): they turn 600 to 1 900
// degrees in all and come back on themselves, bends down to ~1 000 units of radius, two
// stretches 3 800 to 8 000 apart - so a lap that turns less than 600 degrees (a plain round
// loop) is drawn again.
bool draw_bezier_line(Rng& rng, const TrackLook& look, std::vector<Point>& out, std::vector<int32_t>& widths,
                      std::string& why) {
    const int32_t room = (kMap / 2 - 30) * kTexel;
    const int32_t rmax = rng.range(room * 2 / 5, room * 4 / 5), rmin = rmax * 35 / 100;
    const int32_t slack = room - rmax;
    const int32_t cx = kMap * kTexel / 2 + rng.range(-slack, slack);
    const int32_t cz = kMap * kTexel / 2 + rng.range(-slack, slack);
    const int k = rng.range(7, 13);
    std::vector<Point> corner;
    std::vector<int32_t> cwidth, cround;   // per corner: the track's width, the rounding (per mille)
    for (int i = 0; i < k; ++i) {
        const int32_t step = 65536 / k;
        const int32_t a = i * step + rng.range(-step * 2 / 5, step * 2 / 5);
        const int32_t r = rng.range(rmin, rmax);
        corner.push_back({cx + fx::mul15(r, fx::sin16(a)), cz + fx::mul15(r, fx::cos16(a))});
        cwidth.push_back(rng.range(0, 1) ? look.width : rng.range(look.width_lo, look.width_hi));
        cround.push_back(rng.range(250, 480));
    }
    // The sides in clean directions. A bearing t points along (sin t, cos t).
    static constexpr int32_t kClean[4] = {0, 4836, 8192, 11548};   // 0, 26.6, 45, 63.4 degrees
    auto snap = [&](int32_t bearing) {
        int32_t best = 0, best_d = INT32_MAX;
        for (int q = 0; q < 4; ++q)
            for (int c = 0; c < 4; ++c) {
                const int32_t cand = (q * 16384 + kClean[c]) & 0xFFFF;
                const int32_t dd = std::abs(static_cast<int32_t>(static_cast<int16_t>(cand - bearing)));
                if (dd < best_d) { best_d = dd; best = cand; }
            }
        return best;
    };
    std::vector<Point> poly(static_cast<size_t>(k));
    poly[0] = corner[0];
    for (int i = 0; i + 2 < k; ++i) {
        const Point& a = corner[static_cast<size_t>(i)];
        const Point& b = corner[static_cast<size_t>(i + 1)];
        const int32_t len = fx::hypot32(b.x - a.x, b.z - a.z);
        const int32_t dir = snap(fx::atan2_bam16(b.x - a.x, b.z - a.z) & 0xFFFF);
        poly[static_cast<size_t>(i + 1)] = {poly[static_cast<size_t>(i)].x + fx::mul15(len, fx::sin16(dir)),
                                            poly[static_cast<size_t>(i)].z + fx::mul15(len, fx::cos16(dir))};
    }
    {
        // The last two sides, from poly[k-2] through poly[k-1] back to poly[0]: two clean
        // directions u, v and lengths la, lb with la u + lb v = gap, the pair nearest the original.
        const Point from = poly[static_cast<size_t>(k - 2)];
        const int64_t gx = poly[0].x - from.x, gz = poly[0].z - from.z;
        const Point& c1 = corner[static_cast<size_t>(k - 2)];
        const Point& c2 = corner[static_cast<size_t>(k - 1)];
        const int32_t want_u = fx::atan2_bam16(c2.x - c1.x, c2.z - c1.z) & 0xFFFF;
        const int32_t want_v = fx::atan2_bam16(corner[0].x - c2.x, corner[0].z - c2.z) & 0xFFFF;
        int64_t best_cost = INT64_MAX;
        Point mid{};
        for (int qu = 0; qu < 16; ++qu)
            for (int qv = 0; qv < 16; ++qv) {
                const int32_t u = (qu / 4 * 16384 + kClean[qu % 4]) & 0xFFFF, v = (qv / 4 * 16384 + kClean[qv % 4]) & 0xFFFF;
                const int64_t ux = fx::sin16(u), uz = fx::cos16(u), vx = fx::sin16(v), vz = fx::cos16(v);
                const int64_t det = ux * vz - uz * vx;   // Q30
                if (std::abs(det) < (int64_t(1) << 26)) continue;   // nearly parallel
                const int64_t la = (gx * vz - gz * vx) * 32768 / det, lb = (ux * gz - uz * gx) * 32768 / det;
                if (la < 6000 || lb < 6000) continue;
                const int64_t cost = std::abs(static_cast<int32_t>(static_cast<int16_t>(u - want_u))) +
                                     std::abs(static_cast<int32_t>(static_cast<int16_t>(v - want_v)));
                if (cost < best_cost) {
                    best_cost = cost;
                    mid = {from.x + static_cast<int32_t>(la * ux / 32768), from.z + static_cast<int32_t>(la * uz / 32768)};
                }
            }
        if (best_cost == INT64_MAX) {
            why = "boucle impossible a fermer";
            return false;
        }
        poly[static_cast<size_t>(k - 1)] = mid;
    }
    // No corner turning more than 120 degrees: past that the two sides run back side by side, and
    // a CHECKP line of one (6 000 units across) reached the other - the autopilot crossed them
    // out of order (RIG 3 and MOON 3, whose closing sides made a hairpin by the grid).
    for (int i = 0; i < k; ++i) {
        const Point& pv = poly[static_cast<size_t>((i + k - 1) % k)];
        const Point& pc = poly[static_cast<size_t>(i)];
        const Point& pn = poly[static_cast<size_t>((i + 1) % k)];
        const int32_t turn = static_cast<int16_t>(fx::atan2_bam16(pn.x - pc.x, pn.z - pc.z) - fx::atan2_bam16(pc.x - pv.x, pc.z - pv.z));
        if (std::abs(turn) > 65536 / 3) {
            why = "coin trop aigu";
            return false;
        }
        // Nor a side under 6 000 units (the closing ones' least): two corners that close make a
        // zigzag, which MOON 3's BUGGY cut, missing its line.
        if (fx::hypot32(pn.x - pc.x, pn.z - pc.z) < 6000) {
            why = "cote trop court";
            return false;
        }
    }
    // Rounded corners joined by straights, densely (a point every ~64 units), the width carried.
    struct Fillet {
        Point t1, h1, h2, t2;   // tangent point in, handles, tangent point out
    };
    std::vector<Fillet> fil(static_cast<size_t>(k));
    for (int i = 0; i < k; ++i) {
        const Point& pv = poly[static_cast<size_t>((i + k - 1) % k)];
        const Point& pc = poly[static_cast<size_t>(i)];
        const Point& pn = poly[static_cast<size_t>((i + 1) % k)];
        const int32_t lin = fx::hypot32(pc.x - pv.x, pc.z - pv.z), lout = fx::hypot32(pn.x - pc.x, pn.z - pc.z);
        if (lin < 1 || lout < 1) {
            why = "cote nul";
            return false;
        }
        // At most half of each side, so that neighbouring corners never overlap.
        const int32_t r = std::min(lin, lout) / 2 * cround[static_cast<size_t>(i)] / 500;
        auto along = [](const Point& a, const Point& b, int32_t len, int32_t d) {   // from a towards b, d units
            return Point{a.x + static_cast<int32_t>(int64_t(b.x - a.x) * d / len), a.z + static_cast<int32_t>(int64_t(b.z - a.z) * d / len)};
        };
        Fillet f;
        f.t1 = along(pc, pv, lin, r);
        f.t2 = along(pc, pn, lout, r);
        f.h1 = along(pc, pv, lin, r * 45 / 100);   // handles 55 % of the way to the corner
        f.h2 = along(pc, pn, lout, r * 45 / 100);
        fil[static_cast<size_t>(i)] = f;
    }
    std::vector<Point> dense;
    std::vector<int32_t> dense_w;
    auto push_segment = [&](const Point& a, const Point& b, int32_t wa, int32_t wb) {
        const int32_t len = fx::hypot32(b.x - a.x, b.z - a.z);
        const int steps = std::max(1, len / 64);
        for (int s2 = 0; s2 < steps; ++s2) {
            dense.push_back({a.x + static_cast<int32_t>(int64_t(b.x - a.x) * s2 / steps), a.z + static_cast<int32_t>(int64_t(b.z - a.z) * s2 / steps)});
            dense_w.push_back(static_cast<int32_t>(wa + int64_t(wb - wa) * s2 / steps));
        }
    };
    for (int i = 0; i < k; ++i) {
        const Fillet& f = fil[static_cast<size_t>(i)];
        const int32_t w = cwidth[static_cast<size_t>(i)], wn = cwidth[static_cast<size_t>((i + 1) % k)];
        constexpr int64_t T = 64;
        for (int64_t t = 0; t < T; ++t) {
            const int64_t u = T - t;
            auto bz = [&](int64_t p0, int64_t p1, int64_t p2, int64_t p3) {
                return static_cast<int32_t>((p0 * u * u * u + 3 * p1 * u * u * t + 3 * p2 * u * t * t + p3 * t * t * t) / (T * T * T));
            };
            dense.push_back({bz(f.t1.x, f.h1.x, f.h2.x, f.t2.x), bz(f.t1.z, f.h1.z, f.h2.z, f.t2.z)});
            dense_w.push_back(w);
        }
        push_segment(f.t2, fil[static_cast<size_t>((i + 1) % k)].t1, w, wn);
    }
    // Resampled at a constant step along the curve, the width with it.
    out.clear();
    widths.clear();
    out.push_back(dense[0]);
    widths.push_back(dense_w[0]);
    int32_t carry = 0;
    for (size_t i = 0; i < dense.size(); ++i) {
        const Point a = dense[i], b = dense[(i + 1) % dense.size()];
        const int32_t len = fx::hypot32(b.x - a.x, b.z - a.z);
        int32_t at = kSampleStep - carry;
        while (at <= len) {
            out.push_back({a.x + static_cast<int32_t>(static_cast<int64_t>(b.x - a.x) * at / std::max(len, 1)),
                           a.z + static_cast<int32_t>(static_cast<int64_t>(b.z - a.z) * at / std::max(len, 1))});
            widths.push_back(dense_w[i]);
            at += kSampleStep;
        }
        carry = len - (at - kSampleStep);
    }
    if (out.size() > 1) {
        out.pop_back();
        widths.pop_back();
    }
    const int n = static_cast<int>(out.size());
    int32_t widest = 0;
    for (int32_t w : widths) widest = std::max(widest, w);
    const int32_t side = widest / 2 + look.banks_width();

    // 1. Inside the map, with room for the edge and the banks.
    const int32_t lo = 20 * kTexel + side, hi = (kMap - 20) * kTexel - side;
    for (const Point& p : out)
        if (p.x < lo || p.z < lo || p.x > hi || p.z > hi) {
            why = "sort de la carte";
            return false;
        }
    // 2. Neither a sprint nor a marathon.
    if (n * kSampleStep < 100000 || n * kSampleStep > 200000) {
        why = "longueur hors bornes";
        return false;
    }
    // 3. No bend tighter than kMinRadius, measured over four samples (a kilo-unit of road).
    constexpr int32_t kMinRadius = 1000;
    const int32_t max_turn4 = static_cast<int32_t>(int64_t(4) * kSampleStep * 10430 / kMinRadius);
    std::vector<int32_t> heading(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Point& a = out[static_cast<size_t>(i)];
        const Point& b = out[static_cast<size_t>((i + 1) % n)];
        heading[static_cast<size_t>(i)] = fx::atan2_bam16(b.x - a.x, b.z - a.z);
    }
    int64_t turning = 0;
    for (int i = 0; i < n; ++i) {
        const int16_t d = static_cast<int16_t>(heading[static_cast<size_t>((i + 4) % n)] - heading[static_cast<size_t>(i)]);
        if (std::abs(static_cast<int32_t>(d)) > max_turn4) {
            why = "virage trop serre";
            return false;
        }
        turning += std::abs(static_cast<int32_t>(static_cast<int16_t>(heading[static_cast<size_t>((i + 1) % n)] - heading[static_cast<size_t>(i)])));
    }
    // 4. A lap that comes back on itself: 600 degrees of turning at least.
    if (turning < int64_t(65536) * 600 / 360) {
        why = "boucle trop ronde";
        return false;
    }
    // 5. Two stretches that are not neighbours along the road keep grass between their banks.
    const int32_t separation = std::max<int32_t>(5000, 2 * side + 2000);
    const int near_samples = std::max(24, separation * 2 / kSampleStep);
    for (int i = 0; i < n; i += 2)
        for (int j = i + near_samples; j < n; j += 2) {
            if (n - (j - i) < near_samples) continue;
            const int64_t dx = out[static_cast<size_t>(i)].x - out[static_cast<size_t>(j)].x;
            const int64_t dz = out[static_cast<size_t>(i)].z - out[static_cast<size_t>(j)].z;
            if (dx * dx + dz * dz < int64_t(separation) * separation) {
                why = "deux troncons trop proches";
                return false;
            }
        }
    return true;
}

// Rotates the loop so that sample 0 starts the straightest stretch of road: the grid lies
// behind it and the first bend in front of it. Returns the sample that became 0.
int start_on_a_straight(std::vector<Point>& c) {
    const int n = static_cast<int>(c.size());
    std::vector<int32_t> heading(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Point& a = c[static_cast<size_t>(i)];
        const Point& b = c[static_cast<size_t>((i + 1) % n)];
        heading[static_cast<size_t>(i)] = fx::atan2_bam16(b.x - a.x, b.z - a.z);
    }
    int best = 0;
    int64_t best_turn = INT64_MAX;
    for (int i = 0; i < n; ++i) {
        int64_t turn = 0;
        for (int k = -24; k < 8; ++k) {   // 6 000 units behind, 2 000 in front
            const int a = (i + k + n) % n, b = (a + 1) % n;
            turn += std::abs(static_cast<int32_t>(static_cast<int16_t>(
                heading[static_cast<size_t>(b)] - heading[static_cast<size_t>(a)])));
        }
        if (turn < best_turn) {
            best_turn = turn;
            best = i;
        }
    }
    std::rotate(c.begin(), c.begin() + best, c.end());
    return best;
}

// A smooth random field on the map, heights in world units around 0: two octaves of value noise.
std::vector<int32_t> noise_field(Rng& rng, int32_t amp_big, int32_t amp_small) {
    std::vector<int32_t> f(static_cast<size_t>(kMap) * kMap, 0);
    auto octave = [&](int cell, int32_t amp) {
        const int n = kMap / cell + 2;
        std::vector<int32_t> lattice(static_cast<size_t>(n) * n);
        for (int32_t& v : lattice) v = rng.range(-amp, amp);
        for (int gz = 0; gz < kMap; ++gz)
            for (int gx = 0; gx < kMap; ++gx) {
                const int lx = gx / cell, lz = gz / cell;
                const int32_t wx = smooth1024(gx % cell, cell), wz = smooth1024(gz % cell, cell);
                auto L = [&](int x, int z) { return static_cast<int64_t>(lattice[static_cast<size_t>(z) * n + x]); };
                const int64_t top = L(lx, lz) * (1024 - wx) + L(lx + 1, lz) * wx;
                const int64_t bot = L(lx, lz + 1) * (1024 - wx) + L(lx + 1, lz + 1) * wx;
                f[static_cast<size_t>(gz) * kMap + gx] +=
                    static_cast<int32_t>((top * (1024 - wz) + bot * wz) / (1024 * 1024));
            }
    };
    octave(48, amp_big);
    octave(12, amp_small);
    return f;
}

} // namespace

// ---------------------------------------------------------------------------------------------
namespace {

// THE SOURCE ROAD: a shipped circuit's road, every 256 units round its lap - where its middle
// really is, which way it runs, and how high it is. The gate centres only approximate the
// middle (a gate reaches well past both verges, and not symmetrically), so each sample is
// moved across to the middle of the run of road code it stands in, and the moves smoothed.
struct SourceRoad {
    std::vector<Point> c;
    std::vector<int32_t> heading;   // BAM16, the direction of travel
    std::vector<int32_t> h;         // the ground under the middle, world units up
    const Blob* mph = nullptr;
    const Blob* mpc = nullptr;
    int w = 0;
    int32_t height(int32_t x, int32_t z) const {
        const int gx = std::clamp(x / kTexel, 0, w - 2), gz = std::clamp(z / kTexel, 0, w - 2);
        const int64_t fx_ = std::clamp(x - gx * kTexel, 0, kTexel), fz = std::clamp(z - gz * kTexel, 0, kTexel);
        auto H = [&](int cx, int cz) { return static_cast<int64_t>((*mph)[static_cast<size_t>(cz) * w + cx]) * kByte; };
        const int64_t top = H(gx, gz) * (kTexel - fx_) + H(gx + 1, gz) * fx_;
        const int64_t bot = H(gx, gz + 1) * (kTexel - fx_) + H(gx + 1, gz + 1) * fx_;
        return static_cast<int32_t>((top * (kTexel - fz) + bot * fz) / (int64_t(kTexel) * kTexel));
    }
    int code(int32_t x, int32_t z) const {
        const int gx = x / kTexel, gz = z / kTexel;
        if (x < 0 || z < 0 || gx >= w || gz >= w) return -1;
        return (*mpc)[static_cast<size_t>(gz) * w + gx];
    }
};

SourceRoad source_road(const Archive& lev, uint8_t road_mpc) {
    SourceRoad r;
    r.mph = find_member(lev, "MAP.MPH");
    r.mpc = find_member(lev, "MAP.MPC");
    const Blob* cse = find_member(lev, "MAP.CSE");
    if (!r.mph || !r.mpc || !cse) return r;
    r.w = isqrt64(static_cast<int64_t>(r.mph->size()));
    const std::vector<Gate> gates = parse_gates(cse->data(), cse->size());
    for (size_t g = 0; g < gates.size(); ++g) {
        const Point a = gate_centre(gates[g]), b = gate_centre(gates[(g + 1) % gates.size()]);
        const int32_t len = fx::hypot32(b.x - a.x, b.z - a.z);
        const int steps = std::max(1, len / kSampleStep);
        for (int k = 0; k < steps; ++k) r.c.push_back({a.x + (b.x - a.x) * k / steps, a.z + (b.z - a.z) * k / steps});
    }
    const int m = static_cast<int>(r.c.size());
    if (m < 8) return r;
    auto headings = [&]() {
        r.heading.assign(static_cast<size_t>(m), 0);
        for (int i = 0; i < m; ++i) {
            const Point& a = r.c[static_cast<size_t>((i + m - 2) % m)];
            const Point& b = r.c[static_cast<size_t>((i + 2) % m)];
            r.heading[static_cast<size_t>(i)] = fx::atan2_bam16(b.x - a.x, b.z - a.z) & 0xFFFF;
        }
    };
    headings();
    // Across (the left normal): (cos t, -sin t) for a travel heading t.
    std::vector<int32_t> shift(static_cast<size_t>(m), 0);
    for (int i = 0; i < m; ++i) {
        const int32_t t = r.heading[static_cast<size_t>(i)];
        const int32_t nx = fx::cos16(t), nz = -fx::sin16(t);
        int best = INT32_MAX, best_mid = 0;
        int run_start = INT32_MIN;
        for (int32_t o = -4000; o <= 4064; o += 64) {
            const bool on = o <= 4000 && r.code(r.c[static_cast<size_t>(i)].x + fx::mul15(o, nx), r.c[static_cast<size_t>(i)].z + fx::mul15(o, nz)) == road_mpc;
            if (on && run_start == INT32_MIN) run_start = o;
            if (!on && run_start != INT32_MIN) {
                const int mid = (run_start + o - 64) / 2;
                if (std::abs(mid) < best) {
                    best = std::abs(mid);
                    best_mid = mid;
                }
                run_start = INT32_MIN;
            }
        }
        shift[static_cast<size_t>(i)] = best == INT32_MAX ? 0 : best_mid;
    }
    std::vector<Point> moved(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
        int64_t sum = 0;
        for (int k = -4; k <= 4; ++k) sum += shift[static_cast<size_t>((i + k + m) % m)];
        const int32_t o = static_cast<int32_t>(sum / 9);
        const int32_t t = r.heading[static_cast<size_t>(i)];
        moved[static_cast<size_t>(i)] = {r.c[static_cast<size_t>(i)].x + fx::mul15(o, fx::cos16(t)),
                                         r.c[static_cast<size_t>(i)].z - fx::mul15(o, fx::sin16(t))};
    }
    r.c = moved;
    headings();
    for (const Point& p : r.c) r.h.push_back(r.height(p.x, p.z));
    return r;
}

// THE DONOR'S WATER: the MAP.MPC codes that are the sea and its shore tiles. Every shipped map
// keeps them FLAT at the sea's own level, shore tiles included - it is the land behind a shore
// that rises (DIRT1's inner lake: 96 in the middle, 97..108 round it, all at height 0). Found as
// the outside code of an island, plus every code that touches it on at least 30 % of its cells
// and stands at the sea's level on 90 % of them. None on a map without a coast.
std::array<bool, 256> water_codes(const DonorInfo& info, const SourceRoad& src) {
    std::array<bool, 256> w{};
    if (info.coast_distance <= 0 || !src.mph || !src.mpc) return w;
    const int n = src.w;
    std::array<int, 256> count{}, touching{}, level{};
    for (int gz = 0; gz < n; ++gz)
        for (int gx = 0; gx < n; ++gx) {
            const size_t k = static_cast<size_t>(gz) * n + gx;
            const uint8_t c = (*src.mpc)[k];
            ++count[c];
            if (std::abs(static_cast<int>((*src.mph)[k]) - info.outside_height) <= 2) ++level[c];
            bool t = false;
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& o : nb) {
                const int x = gx + o[0], z = gz + o[1];
                if (x >= 0 && z >= 0 && x < n && z < n && (*src.mpc)[static_cast<size_t>(z) * n + x] == info.field_mpc) t = true;
            }
            if (t) ++touching[c];
        }
    for (int c = 0; c < 256; ++c) {
        if (count[c] < 10 || level[c] * 10 < count[c] * 9) continue;
        if (c == info.field_mpc || touching[c] * 10 >= count[c] * 3) w[static_cast<size_t>(c)] = true;
    }
    return w;
}

// THE RELIEF'S RULES, applied after the landscape is laid (user, 2026-09-30: "c'était un
// problème de dénivelé d'une case à l'autre, il arrivait même que tu en génères dans l'eau,
// alors que c'est censé être toujours plat"). The corridor copies the donor's ground relative
// to the NEAREST sample of our road; inside a bend two stretches meet half way and the copy tears
// - 80-byte spikes beside the road, a road tilted 100 % across. Three rules, each one what every
// shipped map obeys:
//
//  1. THE ROAD IS LEVEL ACROSS: every road texel takes its road sample's height exactly.
//  2. WATER IS FLAT, at the sea's level, shore tiles included. Water copied where no bank could
//     reach it from the road within the slope below turns back into land.
//  3. NO STEP between two neighbours steeper than the slope: kSlopeNear bytes a texel within
//     kNearRoad of the road's edge, kSlopeFar beyond. Enforced between the anchors (road, water)
//     by propagation - peaks planed down, pits filled up - so nothing anchored ever moves.
//
// Integer and order-fixed, like the rest of the generator. Returns a line for the report.
std::string constrain_relief(Blob& mph, Blob& mpc, const std::vector<int32_t>& cell_d,
                             const std::vector<int32_t>& cell_r, int32_t half_w, const DonorInfo& info,
                             const SourceRoad& src, const GroundTiles& tiles, std::vector<uint8_t>& road_out,
                             std::vector<uint8_t>& water_out, std::array<bool, 256>& road_code_out) {
    constexpr int kSlopeNear = 14, kSlopeFar = 40;   // bytes a texel: 44 % and 125 %
    constexpr int32_t kNearRoad = 4000;
    constexpr int kNone = 0, kRoad = 1, kWater = 2;
    const size_t cells = static_cast<size_t>(kMap) * kMap;
    std::vector<uint8_t> anchor(cells, kNone);
    std::vector<int> h(cells);
    std::vector<uint8_t> slope(cells);
    for (size_t k = 0; k < cells; ++k) {
        h[k] = mph[k];
        slope[k] = static_cast<uint8_t>(cell_d[k] < half_w + kNearRoad ? kSlopeNear : kSlopeFar);
        if (cell_d[k] <= half_w + kTexel / 2) {
            anchor[k] = kRoad;
            h[k] = std::clamp((cell_r[k] + kByte / 2) / kByte, 1, 254);
        }
    }
    auto step_between = [&](size_t a, size_t b) { return static_cast<int>(std::min(slope[a], slope[b])); };
    // One sweep each way over the grid, relaxing `v` towards its 4 neighbours: `up` keeps
    // v[k] >= v[n] - s (pits filled), otherwise v[k] <= v[n] + s (peaks planed). Cells where
    // `fixed` is set are never written. Returns whether anything moved.
    auto sweep = [&](std::vector<int>& v, const std::vector<uint8_t>& fixed, bool up) {
        bool moved = false;
        for (int pass = 0; pass < 2; ++pass) {
            const int dir = pass == 0 ? 1 : -1;
            for (int gz = pass == 0 ? 0 : kMap - 1; gz >= 0 && gz < kMap; gz += dir)
                for (int gx = pass == 0 ? 0 : kMap - 1; gx >= 0 && gx < kMap; gx += dir) {
                    const size_t k = static_cast<size_t>(gz) * kMap + gx;
                    if (fixed[k]) continue;
                    const int nbx[2] = {gx - dir, gx}, nbz[2] = {gz, gz - dir};
                    for (int q = 0; q < 2; ++q) {
                        if (nbx[q] < 0 || nbz[q] < 0 || nbx[q] >= kMap || nbz[q] >= kMap) continue;
                        const size_t n = static_cast<size_t>(nbz[q]) * kMap + nbx[q];
                        const int s = step_between(k, n);
                        if (up && v[k] < v[n] - s) { v[k] = v[n] - s; moved = true; }
                        if (!up && v[k] > v[n] + s) { v[k] = v[n] + s; moved = true; }
                    }
                }
        }
        return moved;
    };
    // What the road alone allows each texel: no lower than LB, no higher than UB.
    std::vector<uint8_t> is_road(cells);
    std::vector<int> lb(cells, -100000), ub(cells, 100000);
    for (size_t k = 0; k < cells; ++k)
        if (anchor[k] == kRoad) { is_road[k] = 1; lb[k] = ub[k] = h[k]; }
    for (int i = 0; i < 64 && sweep(lb, is_road, true); ++i) {}
    for (int i = 0; i < 64 && sweep(ub, is_road, false); ++i) {}
    // Rule 1's ground: a road texel keeps the code the corridor copied only if that code IS road
    // on the donor - on at least half of its cells within half_w of the donor's own road. The
    // corridor is aligned on the donor's road centre, not on its width: WATER1's river is wider
    // than ours, and its banks landed on our road, where the INFLATE could not float.
    std::array<bool, 256> road_code{};
    road_code[info.road_mpc] = true;
    if (src.mpc && src.c.size() > 8) {
        const int n = src.w;
        std::vector<uint8_t> on(static_cast<size_t>(n) * n);
        const int reach = half_w / kTexel + 1;
        for (const Point& p : src.c) {
            const int cx = p.x / kTexel, cz = p.z / kTexel;
            for (int gz = std::max(0, cz - reach); gz <= std::min(n - 1, cz + reach); ++gz)
                for (int gx = std::max(0, cx - reach); gx <= std::min(n - 1, cx + reach); ++gx) {
                    const int64_t dx = gx * kTexel + kTexel / 2 - p.x, dz = gz * kTexel + kTexel / 2 - p.z;
                    if (dx * dx + dz * dz <= int64_t(half_w) * half_w) on[static_cast<size_t>(gz) * n + gx] = 1;
                }
        }
        std::array<int, 256> all{}, road_side{};
        for (size_t k = 0; k < on.size(); ++k) {
            ++all[(*src.mpc)[k]];
            if (on[k]) ++road_side[(*src.mpc)[k]];
        }
        for (int c = 0; c < 256; ++c)
            if (road_side[c] >= 10 && road_side[c] * 2 >= all[c]) road_code[static_cast<size_t>(c)] = true;
    }
    int repainted = 0;
    for (size_t k = 0; k < cells; ++k)
        if (anchor[k] == kRoad && !road_code[mpc[k]]) {
            mpc[k] = info.road_mpc;
            ++repainted;
        }
    // Rule 2: the water, where the banks can reach it. The tiles say which codes are the sea's
    // (one corner of it is enough: the shore tiles); without them, the donor's heights do.
    std::array<bool, 256> water = water_codes(info, src);
    if (tiles.valid())
        for (int c = 0; c < 256; ++c) water[static_cast<size_t>(c)] = tiles.is_water(static_cast<uint8_t>(c));
    const int sea = std::clamp(tiles.valid() && tiles.sea >= 0 ? tiles.sea_level : info.outside_height, 1, 254);
    int kept = 0, dried = 0;
    for (size_t k = 0; k < cells; ++k) {
        if (anchor[k] != kNone || !water[mpc[k]]) continue;
        if (lb[k] <= sea && sea <= ub[k]) {
            anchor[k] = kWater;
            h[k] = sea;
            ++kept;
        } else {
            mpc[k] = info.land_mpc;
            ++dried;
        }
    }
    // Rule 3: the slope, between the anchors.
    int worst_before = 0;
    for (int gz = 0; gz < kMap; ++gz)
        for (int gx = 0; gx + 1 < kMap; ++gx) {
            const size_t k = static_cast<size_t>(gz) * kMap + gx;
            worst_before = std::max({worst_before, std::abs(h[k] - h[k + 1]),
                                     gz + 1 < kMap ? std::abs(h[k] - h[k + kMap]) : 0});
        }
    std::vector<uint8_t> fixed(cells);
    for (size_t k = 0; k < cells; ++k) fixed[k] = anchor[k] != kNone ? 1 : 0;
    int rounds = 0;
    for (; rounds < 32; ++rounds) {
        bool moved = false;
        for (int i = 0; i < 64 && sweep(h, fixed, false); ++i) moved = true;
        for (int i = 0; i < 64 && sweep(h, fixed, true); ++i) moved = true;
        if (!moved) break;
    }
    int worst_after = 0;
    for (int gz = 0; gz < kMap; ++gz)
        for (int gx = 0; gx + 1 < kMap; ++gx) {
            const size_t k = static_cast<size_t>(gz) * kMap + gx;
            worst_after = std::max({worst_after, std::abs(h[k] - h[k + 1]),
                                    gz + 1 < kMap ? std::abs(h[k] - h[k + kMap]) : 0});
        }
    for (size_t k = 0; k < cells; ++k) mph[k] = static_cast<uint8_t>(std::clamp(h[k], 0, 255));
    road_out.assign(cells, 0);
    water_out.assign(cells, 0);
    for (size_t k = 0; k < cells; ++k) {
        road_out[k] = anchor[k] == kRoad ? 1 : 0;
        water_out[k] = anchor[k] == kWater ? 1 : 0;
    }
    road_code_out = road_code;
    return "relief : route a plat (" + std::to_string(repainted) + " cases repeintes en route), eau " + std::to_string(kept) + " cases (" + std::to_string(dried) +
           " asséchées), marche max " + std::to_string(worst_before) + " -> " + std::to_string(worst_after) +
           " octets (" + std::to_string(rounds) + " tours)";
}

} // namespace

Track generate(const std::string& game_dir, const std::string& theme_name, uint64_t seed,
               const std::string& name, Method method) {
    const bool layers = method == Method::Layers;
    const Theme* theme = find_theme(theme_name);
    if (!theme) throw std::runtime_error("unknown theme " + theme_name);
    // The theme and the seed together: DIRT 7 and SNOW 7 are different circuits.
    uint64_t mix = seed;
    for (char c : theme->name) mix = mix * 131 + static_cast<uint8_t>(c);
    Rng rng(mix);

    // THE DONOR, drawn among the family's circuits: it lends its shapes, textures and script,
    // AND the landscape the road runs through (the corridor, below) - so that the ground codes
    // painted along the road are read through that same circuit's MAP.CHT. Only a circuit with
    // a gate ring and CHECKP lines can lend (CITY3 has no lines).
    std::vector<std::string> able;
    for (const std::string& lv : theme->levels) {
        try {
            const ArchivePtr a = game_archive(game_dir, lv + ".LEV");
            const Blob* shl_b = find_member(*a, "MAP.SHL");
            if (!find_member(*a, "MAP.CSE") || !shl_b || !find_member(*a, lv + ".TXT")) continue;
            bool has_checkp = false;
            for (const std::string& nm : parse_map_shl(shl_b->data(), shl_b->size()))
                has_checkp = has_checkp || upper(nm) == "CHECKP";
            if (has_checkp) able.push_back(lv);
        } catch (const std::exception&) {
        }
    }
    if (able.empty()) throw std::runtime_error("no circuit of " + theme->name + " can lend");
    const std::string donor_name = able[static_cast<size_t>(rng.range(0, static_cast<int32_t>(able.size()) - 1))];
    const ArchivePtr donor_ptr = game_archive(game_dir, donor_name + ".LEV");
    const Archive& donor = *donor_ptr;
    DonorInfo info = analyse_donor(donor, donor_name);
    const SourceRoad src = source_road(donor, info.road_mpc);
    const GroundTiles tiles = learn_ground_tiles(donor, donor_name, info.coast_distance > 0 ? info.field_mpc : -1);
    // An ISLAND only where the outside is the sea. The coast is found from the ground codes alone,
    // and a map whose far ground is merely another shade read as one - MOON3, WATER3, CHOPPER2:
    // their generated circuits were islands in a sea of sand or rock.
    if (info.coast_distance > 0 && tiles.valid()) {
        const auto& c = tiles.corners[info.field_mpc];
        if (tiles.sea < 0 || c[0] != tiles.sea || c[1] != tiles.sea || c[2] != tiles.sea || c[3] != tiles.sea)
            info.coast_distance = 0;
    }

    Track t;
    t.theme = theme->name;
    t.name = name;
    t.seed = seed;
    t.donor = info;

    // THE LAYERS' LOOK (gen/track_layers.h): the donor's ground tiles, and across its road the
    // track's ground, its width, its banks and the base.
    AutoTiles look_tiles;
    TrackLook look;
    if (layers) {
        look_tiles = learn_autotiles(donor, donor_name);
        look = learn_track_look(look_tiles, member(donor, "MAP.MPC"), member(donor, "MAP.MPH"), src.c, src.heading);
        if (!look.valid()) throw std::runtime_error("the track of " + donor_name + " cannot be read");
    }

    // --- 1. the centre line ------------------------------------------------------------------
    std::string why;
    std::map<std::string, int> whys;
    for (t.attempts = 1; t.attempts <= 2000; ++t.attempts) {
        if (layers ? draw_bezier_line(rng, look, t.centre, t.width, why) : draw_centre_line(rng, t.centre, why)) break;
        ++whys[why];
    }
    if (t.attempts > 2000) {
        std::string all;
        for (const auto& [w, c] : whys) all += " " + w + " x" + std::to_string(c);
        throw std::runtime_error("no centre line after 2000 attempts:" + all);
    }
    const int rotated = start_on_a_straight(t.centre);
    if (!t.width.empty()) std::rotate(t.width.begin(), t.width.begin() + rotated, t.width.end());
    const int n = static_cast<int>(t.centre.size());
    t.length = n * kSampleStep;

    // --- 2. the relief -------------------------------------------------------------------------
    // What every shipped map is made of (measured by --analyse on all 22):
    //  - THE EDGE is a plain at one constant level all round the border (0 on DIRT1, 112 on
    //    DIRT3, 101 on RIG1), the relief being entirely inside;
    //  - on DIRT1-2, SNOW1-3, WATER1 that plain is the OUTSIDE - the sea round an island, its own
    //    ground code, its own height - and the coast runs some 25 000 to 40 000 units from the
    //    road. On CITY, RIG1, MOON1-2 there is no coast: one surface out to the border;
    //  - the road sits at its own height, a few bytes above the sea on the islands.
    // So: land round the road out to a wavy coast, hills on the land, the outside beyond it and
    // along the border.
    const int32_t half_w = info.road_half_width;
    // The distance from every texel to the road, over the whole map: a two-pass chamfer (3 for a
    // side step, 4 for a diagonal), in thirds of a texel.
    std::vector<int32_t> cham(static_cast<size_t>(kMap) * kMap, INT32_MAX / 2);
    for (const Point& p : t.centre)
        cham[static_cast<size_t>(std::clamp(p.z / kTexel, 0, kMap - 1)) * kMap + std::clamp(p.x / kTexel, 0, kMap - 1)] = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? 1 : -1;
        for (int gz = pass == 0 ? 0 : kMap - 1; gz >= 0 && gz < kMap; gz += step)
            for (int gx = pass == 0 ? 0 : kMap - 1; gx >= 0 && gx < kMap; gx += step) {
                int32_t& c = cham[static_cast<size_t>(gz) * kMap + gx];
                const int pz = gz - step, px = gx - step, nx = gx + step;
                auto at = [&](int x, int z) { return (x < 0 || z < 0 || x >= kMap || z >= kMap) ? INT32_MAX / 2 : cham[static_cast<size_t>(z) * kMap + x]; };
                c = std::min({c, at(px, gz) + 3, at(gx, pz) + 3, at(px, pz) + 4, at(nx, pz) + 4});
            }
    }
    auto road_distance = [&](size_t k) { return static_cast<int32_t>(static_cast<int64_t>(cham[k]) * kTexel / 3); };

    // What the stages below read, whichever method made the ground.
    const size_t cells = static_cast<size_t>(kMap) * kMap;
    Blob mph(cells), mpc(cells);
    std::vector<uint8_t> is_land(cells, 1);
    std::vector<int64_t> dist2(cells, INT64_MAX);
    std::vector<int32_t> our_heading(static_cast<size_t>(n));
    int src_start = 0;   // where on the donor's lap our sample 0 is (the corridor)
    std::string relief_report, tiles_report;
    constexpr int32_t kCorrIn = 11000, kCorrOut = 15000;
    if (!layers) {

        const bool island = info.coast_distance > 0;
        const int32_t outside_h = (island ? info.outside_height : info.edge_mean[0]) * kByte;
        const int32_t land_h = info.road_height * kByte;
        const int32_t range = std::clamp(info.max_height - info.road_height, 24, 160) * kByte;
        // THE ROAD'S HEIGHT, BORROWED (the user's method, point 1): the road profile of one of the
        // family's own circuits, laid along ours from a point drawn at random - not stretched, so
        // its slopes stay the real ones. Its window ends where it did not start, so the difference
        // is spread over the lap to close the loop. Lightly smoothed: the profile is read at the
        // gate centres, which are not exactly on the road's crown.
        std::vector<int32_t> road(static_cast<size_t>(n), land_h);
        {
            const std::string& src_level = donor_name;
            const std::vector<int32_t>& prof = src.h;
            if (prof.size() > 16) {
                const int m = static_cast<int>(prof.size());
                src_start = rng.range(0, m - 1);
                const int start = src_start;
                std::vector<int32_t> sorted = prof;
                std::sort(sorted.begin(), sorted.end());
                const int32_t median = sorted[sorted.size() / 2];
                const int32_t seam = prof[static_cast<size_t>((start + n) % m)] - prof[static_cast<size_t>(start)];
                for (int i = 0; i < n; ++i)
                    road[static_cast<size_t>(i)] = land_h + prof[static_cast<size_t>((start + i) % m)] - median -
                                                   static_cast<int32_t>(static_cast<int64_t>(seam) * i / n);
                t.profile_from = src_level + " +" + std::to_string(start * kSampleStep);
            }
            for (int pass = 0; pass < 2; ++pass) {
                std::vector<int32_t> s(static_cast<size_t>(n));
                for (int i = 0; i < n; ++i) {
                    int64_t sum = 0;
                    for (int k = -2; k <= 2; ++k) sum += road[static_cast<size_t>((i + k + n) % n)];
                    s[static_cast<size_t>(i)] = static_cast<int32_t>(sum / 5);
                }
                road = s;
            }
            // Never under the outside (the sea), which the land round it would then hold back.
            for (int32_t& r : road) r = std::max(r, outside_h + 3 * kByte);
            // ⚠️ A profile read over a TUNNEL gives the hill above it, and over a BRIDGE the water
            // below: the source's road is not on its terrain there. Until those stretches are
            // recognised, the gradient is held to 45 %, the steepest the terrain itself shows once
            // they are left out.
            constexpr int32_t kMaxStep = kSampleStep * 45 / 100;
            for (int pass = 0; pass < 8; ++pass)
                for (int i = 0; i < 2 * n; ++i) {
                    const int a = i % n, b = (i + 1) % n;
                    road[static_cast<size_t>(b)] = std::clamp(road[static_cast<size_t>(b)], road[static_cast<size_t>(a)] - kMaxStep,
                                                              road[static_cast<size_t>(a)] + kMaxStep);
                }
            int32_t steepest = 0;
            for (int i = 0; i < n; ++i)
                steepest = std::max(steepest, std::abs(road[static_cast<size_t>((i + 1) % n)] - road[static_cast<size_t>(i)]));
            const auto [lo, hi] = std::minmax_element(road.begin(), road.end());
            if (!t.profile_from.empty())
                t.profile_from += ", denivele " + std::to_string(*hi - *lo) + ", pente max " +
                                  std::to_string(steepest * 100 / kSampleStep) + " %";
        }

        // THE GROUND AGAINST THE ROAD - what makes it "hug" the track as the shipped circuits do
        // (user, 2026-09-29: "les abords ne epousent pas la piste"; and on WATER and CHOPPER "une
        // montagne au lieu d'un creux"). Every texel is the height of the NEAREST stretch of road
        // plus the donor's measured CROSS-SECTION at that distance (DonorInfo::xsec_median): on DIRT1
        // the road runs in a cut, the ground +6 bytes at 1 000 units and +26 at 3 000; on WATER the
        // river runs between banks 70 bytes up; on CHOPPER through a canyon 130 deep. Plus a noise as
        // wide as the donor's own spread at that distance, so the banks are not a lathe's.
        const int32_t road_mean = [&] {
            int64_t sum = 0;
            for (int32_t r : road) sum += r;
            return static_cast<int32_t>(sum / std::max(n, 1));
        }();
        // The cross-section as a function of the distance past the road's edge: bin b's value
        // stands at (b + 0.5) * kXsecStep, and 0 at the edge itself.
        auto xsec_at = [&](const int* table, int32_t d) -> int32_t {
            const int32_t x = std::max<int32_t>(0, d - half_w + DonorInfo::kXsecStep / 2);
            const int32_t pos = x - DonorInfo::kXsecStep / 2;
            if (pos <= 0) return static_cast<int32_t>(static_cast<int64_t>(table[0]) * kByte * x / (DonorInfo::kXsecStep / 2));
            const int b = pos / DonorInfo::kXsecStep;
            if (b >= DonorInfo::kXsecBins - 1) return table[DonorInfo::kXsecBins - 1] * kByte;
            const int32_t f = pos - b * DonorInfo::kXsecStep;
            return (table[b] * (DonorInfo::kXsecStep - f) + table[b + 1] * f) * kByte / DonorInfo::kXsecStep;
        };
        // The nearest stretch of road for every texel within kReach; beyond it, the lap's mean.
        constexpr int32_t kReach = DonorInfo::kXsecBins * DonorInfo::kXsecStep;
        const int reach = kReach / kTexel + 2;
        std::vector<int> nearest(static_cast<size_t>(kMap) * kMap, -1);
        for (int i = 0; i < n; ++i) {
            const Point& p = t.centre[static_cast<size_t>(i)];
            const int cx = p.x / kTexel, cz = p.z / kTexel;
            for (int gz = std::max(0, cz - reach); gz <= std::min(kMap - 1, cz + reach); ++gz)
                for (int gx = std::max(0, cx - reach); gx <= std::min(kMap - 1, cx + reach); ++gx) {
                    const int64_t dx = gx * kTexel - p.x, dz = gz * kTexel - p.z;
                    const size_t k = static_cast<size_t>(gz) * kMap + gx;
                    if (dx * dx + dz * dz < dist2[k]) {
                        dist2[k] = dx * dx + dz * dz;
                        nearest[k] = i;
                    }
                }
        }
        const std::vector<int32_t> wobble = noise_field(rng, 1024, 384);        // +/- 1 spread
        const std::vector<int32_t> coast_wobble = noise_field(rng, 5000, 1500);
        const int32_t coast = std::clamp(info.coast_distance, 6000, 30000);
        std::vector<int32_t> cell_d(cells), cell_r(cells);
        constexpr int32_t kVerge = 1500;   // past the road's edge: the verge the corridor paints
        for (int i = 0; i < n; ++i) {
            const Point& a = t.centre[static_cast<size_t>((i + n - 2) % n)];
            const Point& b = t.centre[static_cast<size_t>((i + 2) % n)];
            our_heading[static_cast<size_t>(i)] = fx::atan2_bam16(b.x - a.x, b.z - a.z) & 0xFFFF;
        }
        for (size_t k = 0; k < cells; ++k) {
            cell_d[k] = nearest[k] >= 0 ? isqrt64(dist2[k]) : road_distance(k);
            cell_r[k] = nearest[k] >= 0 ? road[static_cast<size_t>(nearest[k])] : road_mean;
        }

        // THE MASSIF - the generator's OWN hills (user, 2026-10-01: make the hills rather than copy
        // the donor's). Every shipped map is a flat plain at one level with its relief in one mass
        // round the road, and the donor says how (DonorInfo): the share of land raised at each
        // distance from the road, and how a hill climbs from its foot. The outline: a smooth random
        // field, turned into RANKS so that "the lowest p per mille" is exactly p per mille of the
        // map - raised where its rank is under the donor's share at that distance. The height: the
        // donor's climb at this distance from the outline, with knobs.
        const std::vector<int32_t> outline = noise_field(rng, 1024, 512);
        const std::vector<int32_t> knobs = noise_field(rng, 300, 200);           // +/- 30 %
        std::vector<int32_t> rank(cells);
        {
            std::vector<uint32_t> order(cells);
            for (size_t k = 0; k < cells; ++k) order[k] = static_cast<uint32_t>(k);
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                return outline[a] != outline[b] ? outline[a] < outline[b] : a < b;
            });
            for (size_t i = 0; i < cells; ++i) rank[order[i]] = static_cast<int32_t>(i * 1000 / cells);
        }
        auto raised_share = [&](int32_t d) {
            const int32_t pos = std::max<int32_t>(0, d - DonorInfo::kMassifStep / 2);
            const int b = pos / DonorInfo::kMassifStep;
            if (b >= DonorInfo::kMassifBins - 1) return info.raised_permille[DonorInfo::kMassifBins - 1];
            const int32_t f = pos - b * DonorInfo::kMassifStep;
            return (info.raised_permille[b] * (DonorInfo::kMassifStep - f) + info.raised_permille[b + 1] * f) / DonorInfo::kMassifStep;
        };
        // The distance from the foot, a chamfer in thirds of a texel (0 off the massif).
        std::vector<int32_t> foot(cells);
        for (size_t k = 0; k < cells; ++k) foot[k] = rank[k] < raised_share(cell_d[k]) ? INT32_MAX / 2 : 0;
        // The road's own height spread down its sides at the donor's climb (the ~8 bytes a texel of
        // its first texels): a road carried high stands on a ridge, not on a wall.
        const int32_t climb = std::clamp(info.foot_rise[8] * kByte / 8, 4 * kByte, 20 * kByte);
        std::vector<int32_t> ridge(cells, INT32_MIN / 2);
        for (size_t k = 0; k < cells; ++k)
            if (cell_d[k] <= half_w) ridge[k] = cell_r[k];
        for (int pass = 0; pass < 2; ++pass) {
            const int step = pass == 0 ? 1 : -1;
            for (int gz = pass == 0 ? 0 : kMap - 1; gz >= 0 && gz < kMap; gz += step)
                for (int gx = pass == 0 ? 0 : kMap - 1; gx >= 0 && gx < kMap; gx += step) {
                    const size_t k = static_cast<size_t>(gz) * kMap + gx;
                    auto in = [&](int x, int z) { return x >= 0 && z >= 0 && x < kMap && z < kMap; };
                    auto F = [&](int x, int z) { return in(x, z) ? foot[static_cast<size_t>(z) * kMap + x] : 0; };
                    auto R = [&](int x, int z) { return in(x, z) ? ridge[static_cast<size_t>(z) * kMap + x] : INT32_MIN / 2; };
                    foot[k] = std::min({foot[k], F(gx - step, gz) + 3, F(gx, gz - step) + 3, F(gx - step, gz - step) + 4,
                                        F(gx + step, gz - step) + 4});
                    ridge[k] = std::max({ridge[k], R(gx - step, gz) - climb, R(gx, gz - step) - climb,
                                         R(gx - step, gz - step) - climb * 4 / 3, R(gx + step, gz - step) - climb * 4 / 3});
                }
        }
        auto massif_at = [&](size_t k) {
            const int32_t f = foot[k];                 // thirds of a texel
            const int b = f / 3;
            int32_t rise;
            if (b >= DonorInfo::kFootBins - 1) rise = info.foot_rise[DonorInfo::kFootBins - 1] * kByte;
            else rise = (info.foot_rise[b] * (3 - f % 3) + info.foot_rise[b + 1] * (f % 3)) * kByte / 3;
            rise = rise * (1024 + knobs[k]) / 1024;
            rise = std::min(rise, info.max_excess * kByte);
            return std::max(info.base_height * kByte + rise, ridge[k]);
        };

        std::array<bool, 256> lake_code = water_codes(info, src);
        if (tiles.valid())
            for (int c = 0; c < 256; ++c) lake_code[static_cast<size_t>(c)] = tiles.is_water(static_cast<uint8_t>(c));
        for (int gz = 0; gz < kMap; ++gz)
            for (int gx = 0; gx < kMap; ++gx) {
                const size_t k = static_cast<size_t>(gz) * kMap + gx;
                const int e = std::min(std::min(gx, gz), std::min(kMap - 1 - gx, kMap - 1 - gz));
                const int32_t d = cell_d[k];
                const int32_t r = cell_r[k];
                // Near the road: the donor's cross-section (the cut the road runs in). Past it: the massif.
                int32_t hgt = r + xsec_at(info.xsec_median, d) +
                              static_cast<int32_t>(static_cast<int64_t>(xsec_at(info.xsec_spread, d)) * wobble[k] / 1024);
                const int32_t far_w = smooth1024(d - half_w - 2000, 6000);
                hgt += static_cast<int32_t>(static_cast<int64_t>(massif_at(k) - hgt) * far_w / 1024);
                // THE CORRIDOR - the donor's own ground CODES, carried along our road (the user's
                // method, point 1): a texel at distance d on the left or right of our sample i takes
                // the code that lies at the same distance, on the same side, of the donor's sample
                // start + i - the verges, the river's banks. Its RELIEF is no longer copied: it made
                // concentric terraces and radial streaks, and the hills are now the massif's.
                int32_t corr_w = 0;
                int corr_code = -1;
                if (nearest[k] >= 0 && src.c.size() > 8) {
                    const int i = nearest[k];
                    const int m = static_cast<int>(src.c.size());
                    const int j = (src_start + i) % m;
                    const int32_t ti = our_heading[static_cast<size_t>(i)];
                    const Point& ci = t.centre[static_cast<size_t>(i)];
                    const int64_t side = static_cast<int64_t>(gx * kTexel - ci.x) * fx::cos16(ti) -
                                         static_cast<int64_t>(gz * kTexel - ci.z) * fx::sin16(ti);
                    const int32_t lat = side >= 0 ? d : -d;
                    const int32_t tj = src.heading[static_cast<size_t>(j)];
                    const int32_t sx = src.c[static_cast<size_t>(j)].x + fx::mul15(lat, fx::cos16(tj));
                    const int32_t sz = src.c[static_cast<size_t>(j)].z - fx::mul15(lat, fx::sin16(tj));
                    corr_w = 1024 - smooth1024(d - kCorrIn, kCorrOut - kCorrIn);
                    corr_code = src.code(sx, sz);
                    if (corr_code == 240) corr_code = info.land_mpc;   // no tunnel mouth here
                    // Nor the donor's lakes: they lay in ITS hollows, and in our hills they were pits
                    // (rule 2 pulled the ground down round them). Water stays on the road (a river).
                    if (corr_code >= 0 && d > half_w && lake_code[static_cast<size_t>(corr_code)]) corr_code = info.land_mpc;
                }
                // The coast (islands) and the border: the outside - beyond the corridor.
                int32_t land = smooth1024(e - 4, 14);
                if (island) land = land * (1024 - smooth1024(d - (coast + coast_wobble[k] - 3000), 3000)) / 1024;
                land = std::max(land, corr_w);
                hgt = outside_h + static_cast<int32_t>(static_cast<int64_t>(hgt - outside_h) * land / 1024);
                is_land[k] = land >= 512 ? 1 : 0;
                uint8_t code = is_land[k] ? info.land_mpc : info.field_mpc;
                // The copied codes only as far as the verge and a little past it: further out they
                // were the donor's bands at the same distance - rings round our road. The massif is
                // painted instead (gen/hill_paint.h).
                if (corr_w >= 512 && corr_code >= 0 && d <= half_w + 2 * kVerge) code = static_cast<uint8_t>(corr_code);
                else if (d <= half_w) code = info.road_mpc;
                mph[k] = static_cast<uint8_t>(std::clamp((hgt + kByte / 2) / kByte, 1, 254));
                mpc[k] = code;
            }
        std::vector<uint8_t> road_cells, water_cells;
        std::array<bool, 256> road_codes{};
        relief_report = constrain_relief(mph, mpc, cell_d, cell_r, half_w, info, src, tiles,
                                                           road_cells, water_cells, road_codes);
        tiles_report = lay_ground_tiles(tiles, mph, mpc, road_cells, water_cells, road_codes);
        // Then the AUTOTILER lays them again from the vertex materials - edge variants, the road's
        // dirt along the road, the water's rocky ring, the motifs - when it has shown it understands
        // the donor's painting: the donor replayed on itself comes back 96 % equivalent or more (the
        // right ground, any plain variant; gen/autotile.h). MOON3 and WATER1 (~93 %) keep the above.
        {
            const AutoTiles at = learn_autotiles(donor, donor_name);
            if (at.valid() && at.equivalent_permille >= 960) {
                const Blob laid = mpc;
                std::vector<int8_t> V = vertex_materials(at, laid);
                // THE HILLS' PAINT (gen/hill_paint.h): the massif wears the donor's hill grounds by
                // rank of height, the plain its plain's ground. The road, its verges and the water
                // keep what the corridor gave them; the highest ground holds while the bands between
                // grounds are repaired, so a light top gets its medium ring as on DIRT2.
                const HillPaint hp = learn_hill_paint(at, member(donor, "MAP.MPH"), member(donor, "MAP.MPC"), info.road_mpc,
                                                      info.base_height);
                if (hp.valid()) {
                    constexpr int kV = kMap + 1;
                    std::vector<uint8_t> keep(static_cast<size_t>(kV) * kV, 0);
                    for (int vz = 0; vz < kV; ++vz)
                        for (int vx = 0; vx < kV; ++vx)
                            for (int dz = -1; dz <= 0; ++dz)
                                for (int dx = -1; dx <= 0; ++dx) {
                                    const int gx = vx + dx, gz = vz + dz;
                                    if (gx < 0 || gz < 0 || gx >= kMap || gz >= kMap) continue;
                                    const size_t k = static_cast<size_t>(gz) * kMap + gx;
                                    if (cell_d[k] <= half_w + kVerge || water_cells[k] || road_cells[k])
                                        keep[static_cast<size_t>(vz) * kV + vx] = 1;
                                }
                    std::vector<uint8_t> top;
                    const int painted = paint_hills(hp, V, mph, keep, &top);
                    for (size_t v = 0; v < keep.size(); ++v) keep[v] = keep[v] || top[v];
                    const int repaired = repair_materials(at, V, keep);
                    tiles_report += ", " + hp.report + " : " + std::to_string(painted) + " sommets, " + std::to_string(repaired) + " repares";
                }
                const int changed = lay_autotiles(at, V, mpc);
                tiles_report += ", autotuiles : " + std::to_string(changed) + " cases (" + at.report + ")";
            } else {
                tiles_report += ", autotuiles : non (" + (at.valid() ? at.report : std::string("donneur illisible")) + ")";
            }
        }
    } else {
        // THE LAYERS (gen/track_layers.h): flat, the base everywhere, the track and its banks,
        // the bands no tile joins repaired, then the autotiler's tiles. The height: the donor's
        // plain, the water's level too for now - relief comes later, on top of this.
        const uint8_t level = static_cast<uint8_t>(std::clamp(info.base_height, 1, 254));
        std::fill(mph.begin(), mph.end(), level);
        std::vector<int8_t> V;
        std::vector<uint8_t> locked;
        std::vector<std::vector<uint16_t>> soft;
        const std::string lake_report = paint_layers(look, t.centre, t.width, rng.next(), V, locked, &soft);
        const int repaired = repair_materials(look_tiles, V, locked);
        const int thickened = thicken_rings(look_tiles, V, locked);
        std::fill(mpc.begin(), mpc.end(), look_tiles.fill[static_cast<size_t>(look.base)]);
        // The repair and the ring may have changed vertices since: those lose their soft value.
        for (size_t v = 0; v < V.size(); ++v)
            if (soft[0][v] != kSoftUnknown && soft[static_cast<size_t>(V[v])][v] < 500)
                for (auto& s : soft) s[v] = kSoftUnknown;
        // The true edges only where the blur does not explain the donor's variants: on DIRT2
        // (99.7 %) and DIRT1 (84 %) the blur gave the cleaner edges, on DIRT3 (75 %) the true
        // edge does (user, 2026-10-04: DIRT 2 "très bien", DIRT 1 "très carré").
        constexpr int kBlurUnderstood = 800;
        const bool true_edges = look_tiles.variant_permille < kBlurUnderstood;
        const int laid = lay_autotiles(look_tiles, V, mpc, {}, true, true_edges ? &soft : nullptr);
        relief_report = "relief : plat a " + std::to_string(level);
        tiles_report = "couches : " + look.report + " ; " + lake_report + ", " + std::to_string(repaired) + " sommets repares, " + std::to_string(thickened) + " sommets de rive ajoutes, " +
                       std::to_string(laid) + " cases posees (" + look_tiles.report + ")";
    }
    // The ground's Y at a point, +Y DOWN, as the simulation reads it (nearest texel is enough
    // for placing things: the road is level across).
    auto ground_y = [&](int32_t x, int32_t z) {
        const int gx = std::clamp((x + kTexel / 2) / kTexel, 0, kMap - 1);
        const int gz = std::clamp((z + kTexel / 2) / kTexel, 0, kMap - 1);
        return -static_cast<int32_t>(mph[static_cast<size_t>(gz) * kMap + gx]) * kByte;
    };

    // --- 3. the gate ring ----------------------------------------------------------------------
    // Slabs across the road, about 3 000 units deep, tiling it: each one from its sample to the
    // next gate's, a little deeper so that neighbours overlap as the shipped rings do.
    constexpr int kSamplesPerGate = 12;
    const int ngates = std::max(8, n / kSamplesPerGate);
    constexpr int32_t kGateWidth = 8000;
    std::vector<int> gate_sample(static_cast<size_t>(ngates));
    for (int g = 0; g < ngates; ++g) gate_sample[static_cast<size_t>(g)] = static_cast<int>(static_cast<int64_t>(g) * n / ngates);
    for (int g = 0; g < ngates; ++g) {
        const Point a = t.centre[static_cast<size_t>(gate_sample[static_cast<size_t>(g)])];
        const Point b = t.centre[static_cast<size_t>(gate_sample[static_cast<size_t>((g + 1) % ngates)])];
        const Point m{(a.x + b.x) / 2, (a.z + b.z) / 2};
        const int32_t travel = fx::atan2_bam16(b.x - a.x, b.z - a.z);
        // Forward is (cos A, -sin A) for gate heading A: A = travel - a quarter turn.
        const int32_t A = (travel - 0x4000) & 0xFFFF;
        Gate gt;
        gt.angle = static_cast<int16_t>(A >> 6);
        gt.length = kGateWidth;
        gt.half_depth = static_cast<int16_t>(fx::hypot32(b.x - a.x, b.z - a.z) / 2 + 300);
        gt.half_depth_delta = 0;
        const int32_t half = kGateWidth / 2;
        gt.x = m.x - fx::mul15(half, fx::sin16(A));
        gt.z = m.z - fx::mul15(half, fx::cos16(A));
        t.gates.push_back(gt);
    }

    // --- 4. MAP.DAT: checkpoint lines, the start, the scenery ----------------------------------
    std::vector<DatRec> recs;
    const std::vector<DatRec> donor_recs = read_dat(member(donor, "MAP.DAT"));
    std::vector<std::string> shl = parse_map_shl(member(donor, "MAP.SHL").data(), member(donor, "MAP.SHL").size());
    auto slot_name = [&](int s) { return (s >= 0 && static_cast<size_t>(s) < shl.size()) ? upper(shl[static_cast<size_t>(s)]) : std::string(); };

    // The lines: one every 4 gates, 0 at gate 0 (the finish), three pieces side by side.
    int nlines = 0;
    if (info.checkp_slot >= 0) {
        nlines = std::max(3, ngates / 4);
        for (int j = 0; j < nlines; ++j) {
            const int g = j * ngates / nlines;
            const int g_next = ((j + 1) % nlines) * ngates / nlines;
            const Gate& gt = t.gates[static_cast<size_t>(g)];
            int32_t A = gate_bam(gt);
            // On the gate's ENTRY face: the line is crossed as the gate is entered.
            const Point c = gate_centre(gt);
            Point entry{c.x - fx::mul15(gt.half_depth - 300, fx::cos16(A)),
                        c.z + fx::mul15(gt.half_depth - 300, fx::sin16(A))};
            // THE LAYERS: on the centre line just inside the gate, ACROSS the road's own heading
            // there. A gate's slab follows the chord of its ~3 000 units of road; in a rounded
            // corner that chord no longer follows the road, and MOON 3's line lay almost along it
            // - the BUGGY drove past it.
            if (layers) {
                const int s0 = (gate_sample[static_cast<size_t>(g)] + 1) % n;
                const Point& pa = t.centre[static_cast<size_t>((s0 + n - 2) % n)];
                const Point& pb = t.centre[static_cast<size_t>((s0 + 2) % n)];
                A = (fx::atan2_bam16(pb.x - pa.x, pb.z - pa.z) - 0x4000) & 0xFFFF;
                entry = t.centre[static_cast<size_t>(s0)];
            }
            for (int piece = -1; piece <= 1; ++piece) {
                DatRec r;
                r.shape = static_cast<int8_t>(info.checkp_slot);
                r.x = entry.x + fx::mul15(piece * 2048, fx::sin16(A));
                r.z = entry.z + fx::mul15(piece * 2048, fx::cos16(A));
                r.y = ground_y(r.x, r.z);
                r.ay = static_cast<int16_t>((A + 0x4000) & 0xFFFF);   // the travel heading: the long axis across
                r.variant = static_cast<uint8_t>(j | (j == nlines - 1 ? 0x80 : 0));
                r.category = info.checkp_category;
                r.aux = static_cast<uint8_t>(g_next);
                recs.push_back(r);
                // Where the donor's lines float (CHOPPER), a STACK of pieces, same number, one
                // every lift up to kLineTop: a helicopter crosses one of them whatever its
                // altitude. One piece alone, on the ground or at the donor's height, was flown
                // over or under - the autopilot's copter measured from 74 to 1 975 units up.
                constexpr int32_t kLineTop = 2400;
                if (info.checkp_lift > 0) {
                    const int32_t step = std::max<int32_t>(info.checkp_lift, 300);
                    for (int32_t up = step; up <= kLineTop; up += step) {
                        r.y -= step;   // +Y down
                        recs.push_back(r);
                    }
                }
            }
        }
    }

    // The grid: the donor's, set in gate 0's frame as the donor sets it in its own.
    const std::string donor_txt(member(donor, donor_name + ".TXT").begin(), member(donor, donor_name + ".TXT").end());
    const StartingGrid donor_grid = parse_grid(donor_txt);
    const Gate& g0 = t.gates[0];
    const int32_t A0 = gate_bam(g0);
    const Point c0 = gate_centre(g0);
    // Forward (cos A, -sin A), across (sin A, cos A).
    int32_t grid_x = c0.x + fx::mul15(info.grid_along, fx::cos16(A0)) + fx::mul15(info.grid_across, fx::sin16(A0));
    int32_t grid_z = c0.z - fx::mul15(info.grid_along, fx::sin16(A0)) + fx::mul15(info.grid_across, fx::cos16(A0));
    int32_t grid_yaw = (A0 + info.grid_yaw_offset) & 0xFFFF;
    // THE LAYERS: the grid ON our centre line, as far behind gate 0 along the road as the donor's
    // is, in the road's own heading there, and no further across than the track's width allows.
    // Set rigidly in gate 0's frame, as above, it fell on the bank wherever the road bent before
    // the gate, or the donor's gate centre was not its river's middle: on a river 1 000 to 1 500
    // units wide the boats started on the sand (WATER, user, 2026-10-04).
    if (layers && n > 8) {
        const int g0s = gate_sample[0];   // gate 0's slab runs from its sample to the next gate's
        const int mid = (g0s + (gate_sample[1 % ngates] - g0s + n) % n / 2) % n;
        const int steps = info.grid_along / kSampleStep;   // negative: behind
        const int i = ((mid + steps) % n + n) % n;
        const Point& p = t.centre[static_cast<size_t>(i)];
        const Point& a = t.centre[static_cast<size_t>((i + n - 2) % n)];
        const Point& b = t.centre[static_cast<size_t>((i + 2) % n)];
        const int32_t A = (fx::atan2_bam16(b.x - a.x, b.z - a.z) - 0x4000) & 0xFFFF;
        const int32_t room = std::max<int32_t>(0, t.width[static_cast<size_t>(i)] / 2 - 768);
        const int32_t across = std::clamp(info.grid_across, -room, room);
        grid_x = p.x + fx::mul15(across, fx::sin16(A));
        grid_z = p.z + fx::mul15(across, fx::cos16(A));
        grid_yaw = (A + info.grid_yaw_offset) & 0xFFFF;
    }

    // The start gantry, moved rigidly from the donor's grid to ours: the countdown lights.
    if (donor_grid.present) {
        const int32_t turn = (grid_yaw - donor_grid.yaw_bam16) & 0xFFFF;
        const Blob& dmph = member(donor, "MAP.MPH");
        const int dw = isqrt64(static_cast<int64_t>(dmph.size()));
        auto donor_ground_y = [&](int32_t x, int32_t z) {
            const int gx = std::clamp((x + kTexel / 2) / kTexel, 0, dw - 1), gz = std::clamp((z + kTexel / 2) / kTexel, 0, dw - 1);
            return -static_cast<int32_t>(dmph[static_cast<size_t>(gz) * dw + gx]) * kByte;
        };
        for (const DatRec& d : donor_recs) {
            const std::string nm = slot_name(d.shape);
            if (nm != "START" && nm != "S_LIGHTS") continue;
            const int32_t vx = d.x - donor_grid.x, vz = d.z - donor_grid.z;
            // Bearing +turn: (sin t, cos t) -> (sin(t+turn), cos(t+turn)).
            DatRec r = d;
            r.x = grid_x + fx::mul15(vx, fx::cos16(turn)) + fx::mul15(vz, fx::sin16(turn));
            r.z = grid_z - fx::mul15(vx, fx::sin16(turn)) + fx::mul15(vz, fx::cos16(turn));
            r.y = d.y - donor_ground_y(d.x, d.z) + ground_y(r.x, r.z);
            r.ay = static_cast<int16_t>((d.ay + turn) & 0xFFFF);
            recs.push_back(r);
        }
    }

    // THE ROADSIDE, carried with the corridor: every donor object within the corridor - barriers,
    // signs, chevrons, ramps, trees - moved from its place beside the donor's road to the same
    // place beside ours, turned by the difference of the two roads' headings, and set on our
    // ground as high as it stood above the donor's. Not the start pieces (ours are placed
    // above), not CHECKP, not the tunnels (131, 250: they need the terrain's holes).
    int carried = 0;
    if (!layers && src.c.size() > 8) {
        const int m = static_cast<int>(src.c.size());
        for (const DatRec& d : donor_recs) {
            const std::string nm = slot_name(d.shape);
            if (d.shape == info.checkp_slot || nm.empty() || nm == "START" || nm.rfind("S_", 0) == 0) continue;
            if (d.category == 131 || d.category == 250) continue;
            int j = 0;
            int64_t best = INT64_MAX;
            for (int q = 0; q < m; ++q) {
                const int64_t dx = d.x - src.c[static_cast<size_t>(q)].x, dz = d.z - src.c[static_cast<size_t>(q)].z;
                if (dx * dx + dz * dz < best) {
                    best = dx * dx + dz * dz;
                    j = q;
                }
            }
            const int32_t tj = src.heading[static_cast<size_t>(j)];
            const int32_t vx = d.x - src.c[static_cast<size_t>(j)].x, vz = d.z - src.c[static_cast<size_t>(j)].z;
            // Along (sin t, cos t), across (cos t, -sin t).
            const int32_t along = fx::mul15(vx, fx::sin16(tj)) + fx::mul15(vz, fx::cos16(tj));
            const int32_t across = fx::mul15(vx, fx::cos16(tj)) - fx::mul15(vz, fx::sin16(tj));
            if (std::abs(across) > kCorrIn - 500) continue;
            for (int i = (j - src_start + m) % m; i < n; i += m) {
                const int32_t ti = our_heading[static_cast<size_t>(i)];
                const Point& ci = t.centre[static_cast<size_t>(i)];
                DatRec r = d;
                r.x = ci.x + fx::mul15(along, fx::sin16(ti)) + fx::mul15(across, fx::cos16(ti));
                r.z = ci.z + fx::mul15(along, fx::cos16(ti)) - fx::mul15(across, fx::sin16(ti));
                // Never ON our road: inside our bends the donor's roadside folds onto it, and on
                // CITY a building between two of the donor's streets lands on ours.
                {
                    const int gx = std::clamp(r.x / kTexel, 0, kMap - 1), gz = std::clamp(r.z / kTexel, 0, kMap - 1);
                    const int64_t d2 = dist2[static_cast<size_t>(gz) * kMap + gx];
                    if (d2 < int64_t(half_w + 900) * (half_w + 900)) continue;
                }
                const int32_t donor_ground = -src.height(d.x, d.z);
                r.y = d.y - donor_ground + ground_y(r.x, r.z);
                r.ay = static_cast<int16_t>((d.ay + ti - tj) & 0xFFFF);
                recs.push_back(r);
                ++carried;
            }
        }
    }

    // The scenery: the donor's ordinary decor, scattered off the road. Only the plain categories
    // - no tunnels, no animated pieces, no terrain-hugging barriers, no start pieces.
    std::vector<size_t> templates;
    for (size_t i = 0; i < donor_recs.size(); ++i) {
        const DatRec& d = donor_recs[i];
        const std::string nm = slot_name(d.shape);
        if (d.shape == info.checkp_slot || nm.empty()) continue;
        if (d.category > 5 && d.category != 100 && d.category != 128 && d.category != 129) continue;
        if (nm.rfind("S_", 0) == 0 || nm == "START") continue;
        templates.push_back(i);
    }
    int placed = 0;
    if (!layers && !templates.empty()) {
        const int wanted = 250;
        for (int tries = 0; tries < wanted * 6 && placed < wanted; ++tries) {
            const int gx = rng.range(20, kMap - 21), gz = rng.range(20, kMap - 21);
            const size_t k = static_cast<size_t>(gz) * kMap + gx;
            if (!is_land[k] || road_distance(k) < kCorrOut) continue;   // the corridor has its own
            DatRec r = donor_recs[templates[static_cast<size_t>(rng.range(0, static_cast<int32_t>(templates.size()) - 1))]];
            r.x = gx * kTexel + rng.range(-200, 200);
            r.z = gz * kTexel + rng.range(-200, 200);
            r.y = ground_y(r.x, r.z);
            r.ay = static_cast<int16_t>(rng.range(0, 0xFFFF));
            recs.push_back(r);
            ++placed;
        }
    }

    // --- 5. the script -------------------------------------------------------------------------
    // The donor's, minus what belongs to its own map (cameras, helicopter, start draw, unsafe
    // gates, cars), plus our grid and the family's vehicles.
    std::string script;
    {
        size_t pos = 0;
        while (pos < donor_txt.size()) {
            size_t eol = donor_txt.find('\n', pos);
            if (eol == std::string::npos) eol = donor_txt.size();
            const std::string line = donor_txt.substr(pos, eol - pos + (eol < donor_txt.size() ? 1 : 0));
            pos = eol + 1;
            size_t b = line.find_first_not_of(" \t");
            std::string word;
            if (b != std::string::npos)
                while (b < line.size() && std::isalpha(static_cast<unsigned char>(line[b]))) word += static_cast<char>(std::tolower(static_cast<unsigned char>(line[b++])));
            if (word == "grid" || word == "fixcam" || word == "heli" || word == "spos" ||
                word == "unsafe" || word == "car")
                continue;
            script += line;
        }
    }
    std::vector<std::string> flyby;
    for (const std::string& lv : theme->levels) {
        try {
            const ArchivePtr a = game_archive(game_dir, lv + ".LEV");
            const Blob* txt = find_member(*a, lv + ".TXT");
            if (!txt) continue;
            for (const std::string& c : parse_car_list(std::string(txt->begin(), txt->end()))) {
                if (is_presentation_vehicle(c)) {
                    if (std::find(flyby.begin(), flyby.end(), c) == flyby.end()) flyby.push_back(c);
                    continue;
                }
                if (std::find(t.cars.begin(), t.cars.end(), c) == t.cars.end()) t.cars.push_back(c);
            }
        } catch (const std::exception&) {
        }
    }
    {
        char buf[256];
        script += "\r\n; --- generated: theme " + theme->name + ", seed " + std::to_string(seed) + " ---\r\n";
        for (const std::string& c : t.cars) script += "car " + c + "\r\n";
        for (const std::string& c : flyby) script += "car " + c + "\r\n";
        std::string g = "grid " + std::to_string(grid_x) + ", " + std::to_string(grid_z) + ", " +
                        std::to_string(grid_yaw);
        for (int i = 0; i < 2; ++i) {
            std::snprintf(buf, sizeof buf, ", %d, %d, %d", donor_grid.two[i].a, donor_grid.two[i].b, donor_grid.two[i].dyaw);
            g += buf;
        }
        for (int i = 0; i < 6; ++i) {
            std::snprintf(buf, sizeof buf, ", %d, %d, %d", donor_grid.six[i].a, donor_grid.six[i].b, donor_grid.six[i].dyaw);
            g += buf;
        }
        script += g + "\r\n";
    }

    // --- 6. the files ------------------------------------------------------------------------
    for (const auto& [fname, blob] : donor) {
        const std::string u = upper(fname);
        if (u.size() > 4 && u.compare(u.size() - 4, 4, ".REC") == 0) continue;   // the donor's laps
        if (u == "MAP.GRP" || u == "TOPMAP.SPR" || u == upper(donor_name) + ".TXT") continue;
        t.files[fname] = blob;
    }
    t.files["MAP.MPH"] = std::move(mph);
    t.files["MAP.MPC"] = std::move(mpc);
    t.files["MAP.CSE"] = write_cse(t.gates);
    t.files["MAP.DAT"] = write_dat(recs);
    t.files[name + ".TXT"] = Blob(script.begin(), script.end());

    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%s graine %llu : %d echantillons, %d unites (%d essais), %d portes, %d lignes "
                  "CHECKP, %d objets poses, vehicules",
                  theme->name.c_str(), static_cast<unsigned long long>(seed), n, t.length,
                  t.attempts, ngates, nlines, placed);
    t.report = buf;
    if (!t.profile_from.empty()) t.report += " ; paysage et profil de " + t.profile_from + ", " + std::to_string(carried) + " objets de bord de route;";
    t.report += " " + relief_report + "; " + tiles_report + " [" + tiles.report + "];";
    for (const std::string& c : t.cars) t.report += " " + c;
    // The fingerprint, over the files in NAME order (the archive is a hash map, whose own order
    // is not the same from one build to the next).
    {
        std::vector<std::string> names;
        for (const auto& kv : t.files) names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        uint32_t h = 2166136261u;
        auto eat = [&](uint8_t b) { h = (h ^ b) * 16777619u; };
        for (const std::string& nm : names) {
            for (char ch : nm) eat(static_cast<uint8_t>(ch));
            for (uint8_t b : t.files.at(nm)) eat(b);
        }
        t.fingerprint = h;
        std::snprintf(buf, sizeof buf, ", empreinte %08x", h);
        t.report += buf;
    }
    return t;
}

// ---------------------------------------------------------------------------------------------
std::vector<uint8_t> top_view(const Track& t) {
    const int w = t.map_w, h = t.map_h;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3, 0);
    const Blob* mph = find_member(t.files, "MAP.MPH");
    const Blob* mpc = find_member(t.files, "MAP.MPC");
    if (!mph || !mpc) return px;
    const uint8_t road = t.donor.road_mpc, verge = t.donor.verge_mpc, land = t.donor.land_mpc;
    auto put = [&](int x, int z, int r, int g, int b) {
        if (x < 0 || z < 0 || x >= w || z >= h) return;
        uint8_t* p = &px[(static_cast<size_t>(z) * w + x) * 3];
        p[0] = static_cast<uint8_t>(r);
        p[1] = static_cast<uint8_t>(g);
        p[2] = static_cast<uint8_t>(b);
    };
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            const size_t k = static_cast<size_t>(z) * w + x;
            const int hv = (*mph)[k];
            // Shaded from the north-west, so a slope reads.
            const int nb = (*mph)[static_cast<size_t>(std::max(0, z - 1)) * w + std::max(0, x - 1)];
            const int lit = std::clamp(90 + hv / 2 + (hv - nb) * 6, 0, 255);
            if ((*mpc)[k] == road) put(x, z, 70 + lit / 4, 70 + lit / 4, 70 + lit / 4);
            else if ((*mpc)[k] == verge) put(x, z, lit * 3 / 4, lit * 2 / 3, lit / 3);
            else if ((*mpc)[k] == land) put(x, z, lit / 3, lit * 3 / 4, lit / 3);
            else put(x, z, lit / 5, lit / 3, lit);   // the outside: blue
        }
    for (const Point& p : t.centre) put(p.x / kTexel, p.z / kTexel, 255, 255, 255);
    // Gates: the anchor verge in blue, the far one in cyan, gate 0 in red.
    for (size_t i = 0; i < t.gates.size(); ++i) {
        const Gate& g = t.gates[i];
        const int32_t a = gate_bam(g);
        for (int32_t e = 0; e <= g.length; e += 128) {
            const int32_t x = g.x + fx::mul15(e, fx::sin16(a)), z = g.z + fx::mul15(e, fx::cos16(a));
            if (i == 0) put(x / kTexel, z / kTexel, 255, 40, 40);
            else put(x / kTexel, z / kTexel, 60, 120, 255);
        }
    }
    // The CHECKP lines in yellow, the grid origin in magenta.
    if (const Blob* dat = find_member(t.files, "MAP.DAT")) {
        const Blob* shl = find_member(t.files, "MAP.SHL");
        std::vector<std::string> names;
        if (shl) names = parse_map_shl(shl->data(), shl->size());
        for (const DatRec& r : read_dat(*dat)) {
            const std::string nm = (r.shape >= 0 && static_cast<size_t>(r.shape) < names.size()) ? upper(names[static_cast<size_t>(r.shape)]) : "";
            if (nm == "CHECKP") {
                // The box's long axis is its local X; drawn along the instance's yaw.
                for (int32_t e = -1024; e <= 1024; e += 128) {
                    const int32_t yaw = r.ay;
                    put((r.x + fx::mul15(e, fx::cos16(yaw))) / kTexel, (r.z - fx::mul15(e, fx::sin16(yaw))) / kTexel, 255, 230, 0);
                }
            } else if (nm == "START" || nm == "S_LIGHTS") {
                put(r.x / kTexel, r.z / kTexel, 255, 120, 0);
            } else {
                put(r.x / kTexel, r.z / kTexel, 20, 70, 20);
            }
        }
    }
    const Blob* txt = find_member(t.files, t.name + ".TXT");
    if (txt) {
        const StartingGrid g = parse_grid(std::string(txt->begin(), txt->end()));
        for (int s = 0; s < 6; ++s) {
            const RespawnPoint p = grid_slot(g, 6, s);
            for (int d = -1; d <= 1; ++d) {
                put(p.x / kTexel + d, p.z / kTexel, 255, 0, 255);
                put(p.x / kTexel, p.z / kTexel + d, 255, 0, 255);
            }
        }
    }
    return px;
}

} // namespace bigred::gen
