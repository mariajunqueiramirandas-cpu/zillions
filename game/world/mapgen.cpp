#include "world/mapgen.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "core/jobs.h"
#include "core/rng.h"
#include "math/noise.h"

namespace zl {

namespace {

constexpr f32 kInf = 1.0e9f;
constexpr f32 kDiag = 1.41421356f;

// 4-neighbourhood first, then the diagonals.
constexpr i32 kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr i32 kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

// Radii (in tiles) of the colony start area.
constexpr f32 kStartClear = 6.0f;    // forced open grass
constexpr f32 kStartFlat = 12.0f;    // no lakes or mountains inside
constexpr f32 kStartBlend = 26.0f;   // terrain eases back to natural

// Value at quantile p of `values`. Reorders the vector.
f32 percentile(std::vector<f32>& values, f32 p) {
    if (values.empty()) return 0.0f;
    const size_t k = static_cast<size_t>(saturate(p) * static_cast<f32>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

// Two-pass chamfer distance transform. On entry `d` holds 0 at sources and
// kInf elsewhere; on exit every cell holds its approximate distance in tiles.
void chamfer_distance(std::vector<f32>& d, i32 w, i32 h) {
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y * w + x);
            f32 v = d[i];
            if (x > 0) v = std::min(v, d[i - 1] + 1.0f);
            if (y > 0) {
                const size_t up = i - static_cast<size_t>(w);
                v = std::min(v, d[up] + 1.0f);
                if (x > 0) v = std::min(v, d[up - 1] + kDiag);
                if (x < w - 1) v = std::min(v, d[up + 1] + kDiag);
            }
            d[i] = v;
        }
    }
    for (i32 y = h - 1; y >= 0; --y) {
        for (i32 x = w - 1; x >= 0; --x) {
            const size_t i = static_cast<size_t>(y * w + x);
            f32 v = d[i];
            if (x < w - 1) v = std::min(v, d[i + 1] + 1.0f);
            if (y < h - 1) {
                const size_t dn = i + static_cast<size_t>(w);
                v = std::min(v, d[dn] + 1.0f);
                if (x > 0) v = std::min(v, d[dn - 1] + kDiag);
                if (x < w - 1) v = std::min(v, d[dn + 1] + kDiag);
            }
            d[i] = v;
        }
    }
}

struct Components {
    std::vector<i32> label;  // component id per tile, -1 for non-members
    std::vector<i32> size;   // tile count per component
};

// Labels 4-connected regions of tiles for which member(index) is true.
template <class Pred>
Components label_components(i32 w, i32 h, Pred&& member) {
    Components c;
    c.label.assign(static_cast<size_t>(w * h), -1);
    std::vector<i32> stack;
    for (i32 seed = 0; seed < w * h; ++seed) {
        if (c.label[static_cast<size_t>(seed)] != -1 || !member(seed)) continue;
        const i32 id = static_cast<i32>(c.size.size());
        i32 count = 0;
        c.label[static_cast<size_t>(seed)] = id;
        stack.push_back(seed);
        while (!stack.empty()) {
            const i32 i = stack.back();
            stack.pop_back();
            ++count;
            const i32 x = i % w, y = i / w;
            for (int k = 0; k < 4; ++k) {
                const i32 nx = x + kDx[k], ny = y + kDy[k];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const i32 j = ny * w + nx;
                if (c.label[static_cast<size_t>(j)] != -1 || !member(j)) continue;
                c.label[static_cast<size_t>(j)] = id;
                stack.push_back(j);
            }
        }
        c.size.push_back(count);
    }
    return c;
}

class Generator {
public:
    Generator(const MapGenParams& params, JobSystem& jobs, TileMap& map)
        : p_(params), jobs_(jobs), m_(map), w_(params.size), h_(params.size), n_(params.size * params.size) {}

    void run() {
        const auto phase = [this](const char* name, void (Generator::*step)()) {
            const Stopwatch timer;
            (this->*step)();
            m_.stats.phases.push_back({name, timer.millis()});
        };
        phase("noise fields", &Generator::noise_fields);
        phase("start area", &Generator::shape_start_area);
        phase("lakes", &Generator::lakes);
        phase("rivers", &Generator::rivers);
        phase("mountains", &Generator::mountains);
        phase("classify", &Generator::classify);
        phase("fords", &Generator::fords);
        phase("deposits", &Generator::deposits);
        phase("clear start", &Generator::clear_start);
        phase("connectivity", &Generator::connect_regions);
        phase("finish", &Generator::finish);
    }

private:
    // --- helpers -----------------------------------------------------------

    size_t at(i32 x, i32 y) const { return static_cast<size_t>(y * w_ + x); }
    bool inside(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
    u32 seed(u32 stream) const { return hash32(static_cast<u32>(m_.seed) ^ hash32(static_cast<u32>(m_.seed >> 32) + stream * 0x9E3779B9u)); }
    f32 unit(i32 x, i32 y, u32 stream) const { return hash_to_unit(hash2(x, y, seed(stream))); }

    f32 start_dist(i32 x, i32 y) const {
        const f32 dx = static_cast<f32>(x - m_.start.x), dy = static_cast<f32>(y - m_.start.y);
        return std::sqrt(dx * dx + dy * dy);
    }

    Terrain& terrain(i32 i) { return m_.terrain[static_cast<size_t>(i)]; }
    bool is_water(i32 i) const { return m_.terrain[static_cast<size_t>(i)] == Terrain::Water; }
    bool is_river(i32 i) const { return (m_.flags[static_cast<size_t>(i)] & TileFlag::River) != 0; }

    // Open ground: land a unit could stand on once any trees are gone.
    bool is_open_ground(i32 i) const {
        const Terrain t = m_.terrain[static_cast<size_t>(i)];
        return (t == Terrain::Sand || t == Terrain::Grass || t == Terrain::Dirt || t == Terrain::Forest) &&
               m_.resource[static_cast<size_t>(i)] == Resource::None;
    }

    bool is_walkable(i32 i) const {
        const Terrain t = m_.terrain[static_cast<size_t>(i)];
        return (t == Terrain::Shallows || t == Terrain::Sand || t == Terrain::Grass || t == Terrain::Dirt) &&
               m_.resource[static_cast<size_t>(i)] == Resource::None;
    }

    // --- 1. noise fields ---------------------------------------------------

    // Elevation is low-frequency fBm (rolling land and lake basins) plus ridged
    // noise gated by a very low-frequency mask, so mountains gather into ranges
    // with open country between them. Coordinates are in tiles, which keeps
    // feature sizes constant across map sizes.
    void noise_fields() {
        grove_.resize(static_cast<size_t>(n_));
        const u32 s_warp_x = seed(1), s_warp_y = seed(2), s_base = seed(3), s_range = seed(4);
        const u32 s_ridge = seed(5), s_moist = seed(6), s_grove = seed(7);

        jobs_.parallel_for(h_, 4, [&](i32 y0, i32 y1) {
            for (i32 y = y0; y < y1; ++y) {
                for (i32 x = 0; x < w_; ++x) {
                    const f32 px = static_cast<f32>(x), py = static_cast<f32>(y);

                    // Domain warp: bends features so coasts and ranges meander.
                    const f32 wx = fbm2(px / 150.0f, py / 150.0f, s_warp_x, 3);
                    const f32 wy = fbm2(px / 150.0f, py / 150.0f, s_warp_y, 3);
                    const f32 qx = px + wx * 30.0f, qy = py + wy * 30.0f;

                    const f32 base = fbm2(qx / 120.0f, qy / 120.0f, s_base, 5);
                    const f32 range = smoothstep(-0.30f, 0.30f, fbm2(px / 210.0f, py / 210.0f, s_range, 2));
                    const f32 ridge = ridged2(qx / 95.0f, qy / 95.0f, s_ridge, 4);

                    const size_t i = at(x, y);
                    m_.elevation[i] = saturate(0.30f + 0.28f * base + 0.52f * ridge * ridge * range);
                    m_.moisture[i] = saturate(0.5f + 0.6f * fbm2(px / 100.0f, py / 100.0f, s_moist, 4));
                    grove_[i] = fbm2(px / 23.0f, py / 23.0f, s_grove, 3);
                }
            }
        });

        // Thresholds come from the elevation distribution, so the requested
        // water and mountain coverage is met for every seed.
        std::vector<f32> scratch = m_.elevation;
        m_.sea_level = percentile(scratch, p_.water);
        m_.mountain_level = percentile(scratch, 1.0f - p_.mountains);
        if (m_.mountain_level <= m_.sea_level) m_.mountain_level = m_.sea_level + 1.0e-3f;
    }

    // --- 2. start area -----------------------------------------------------

    // Squeezes elevation around the start into the plains band so the colony
    // always begins on open, level ground.
    void shape_start_area() {
        const f32 band = m_.mountain_level - m_.sea_level;
        const f32 lo = m_.sea_level + 0.15f * band;
        const f32 hi = m_.mountain_level - 0.25f * band;
        const i32 r = static_cast<i32>(kStartBlend) + 1;
        for (i32 y = std::max(0, m_.start.y - r); y <= std::min(h_ - 1, m_.start.y + r); ++y) {
            for (i32 x = std::max(0, m_.start.x - r); x <= std::min(w_ - 1, m_.start.x + r); ++x) {
                const f32 k = 1.0f - smoothstep(kStartFlat, kStartBlend, start_dist(x, y));
                f32& e = m_.elevation[at(x, y)];
                e = lerp(e, clamp(e, lo, hi), k);
            }
        }
    }

    // --- 3. lakes ----------------------------------------------------------

    void lakes() {
        const f32 sea = m_.sea_level;
        // Drop ponds and islets too small to matter for play or to read on screen.
        const Components water = label_components(w_, h_, [&](i32 i) { return m_.elevation[static_cast<size_t>(i)] < sea; });
        for (i32 i = 0; i < n_; ++i) {
            const i32 id = water.label[static_cast<size_t>(i)];
            if (id >= 0 && water.size[static_cast<size_t>(id)] < 12) m_.elevation[static_cast<size_t>(i)] = sea + 1.0e-4f;
        }
        const Components land = label_components(w_, h_, [&](i32 i) { return m_.elevation[static_cast<size_t>(i)] >= sea; });
        for (i32 i = 0; i < n_; ++i) {
            const i32 id = land.label[static_cast<size_t>(i)];
            if (id >= 0 && land.size[static_cast<size_t>(id)] < 8) m_.elevation[static_cast<size_t>(i)] = sea - 1.0e-4f;
        }
        for (i32 i = 0; i < n_; ++i) terrain(i) = m_.elevation[static_cast<size_t>(i)] < sea ? Terrain::Water : Terrain::Grass;
    }

    // --- 4. rivers ---------------------------------------------------------

    // Rivers follow the actual drainage of the heightfield. A priority flood
    // (Barnes et al. 2014) grows inland from every lake and map edge, always
    // expanding the lowest tile on the frontier; each tile drains to the tile
    // it was reached from, which routes water through pits instead of trapping
    // it. Summing upstream area then gives the flow, and tiles with enough
    // flow become river. The result: rivers rise in high ground, merge on the
    // way down, never run uphill, and end in a lake or leave the map.
    void rivers() {
        if (p_.rivers <= 0.0f) return;
        const f32 band = m_.mountain_level - m_.sea_level;

        std::vector<f32> level(static_cast<size_t>(n_));
        std::vector<i32> down(static_cast<size_t>(n_), -1);
        std::vector<u8> closed(static_cast<size_t>(n_), 0);
        std::vector<i32> order;
        order.reserve(static_cast<size_t>(n_));

        using Node = std::pair<f32, i32>;
        std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

        const u32 s_meander = seed(14), s_jitter = seed(8);
        jobs_.parallel_for(h_, 8, [&](i32 y0, i32 y1) {
            for (i32 y = y0; y < y1; ++y) {
                for (i32 x = 0; x < w_; ++x) {
                    const size_t i = at(x, y);
                    // A gentle dome under the colony makes drainage flow around it.
                    const f32 dome = 0.30f * band * (1.0f - smoothstep(4.0f, 20.0f, start_dist(x, y)));
                    // Small bumps in the routing surface make channels meander
                    // instead of running ruler-straight across flat ground.
                    const f32 meander = fbm2(static_cast<f32>(x) / 9.0f, static_cast<f32>(y) / 9.0f, s_meander, 2);
                    level[i] = m_.elevation[i] + dome + meander * 0.07f * band + (hash_to_unit(hash2(x, y, s_jitter)) - 0.5f) * 0.002f;
                    if (is_water(static_cast<i32>(i)) || x == 0 || y == 0 || x == w_ - 1 || y == h_ - 1) closed[i] = 1;
                }
            }
        });
        for (i32 y = 0; y < h_; ++y) {
            for (i32 x = 0; x < w_; ++x) {
                const size_t i = at(x, y);
                if (!closed[i]) continue;
                // Only outlets that touch unvisited land need to seed the flood.
                bool frontier = false;
                for (int k = 0; k < 8 && !frontier; ++k) {
                    const i32 nx = x + kDx[k], ny = y + kDy[k];
                    frontier = inside(nx, ny) && !closed[at(nx, ny)];
                }
                if (frontier) open.push({level[i], static_cast<i32>(i)});
            }
        }

        const f32 flat_step = 0.004f * band;
        while (!open.empty()) {
            const auto [lv, i] = open.top();
            open.pop();
            const i32 x = i % w_, y = i / w_;
            for (int k = 0; k < 8; ++k) {
                const i32 nx = x + kDx[k], ny = y + kDy[k];
                if (!inside(nx, ny)) continue;
                const size_t j = at(nx, ny);
                if (closed[j]) continue;
                closed[j] = 1;
                down[j] = i;
                // Across a pit the flood would otherwise advance as a flat
                // front and drain it in straight parallel lines. A small
                // random rise per step makes those paths wander and merge.
                level[j] = std::max(level[j], lv + flat_step * (0.25f + hash_to_unit(hash2(nx, ny, s_jitter + 1))));
                open.push({level[j], static_cast<i32>(j)});
                order.push_back(static_cast<i32>(j));
            }
        }

        // Tiles were appended parent-first, so walking backwards visits every
        // tile after all of its upstream tiles.
        std::vector<f32> flow(static_cast<size_t>(n_));
        for (i32 i = 0; i < n_; ++i) flow[static_cast<size_t>(i)] = is_water(i) ? 0.0f : 1.0f;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            flow[static_cast<size_t>(down[static_cast<size_t>(*it)])] += flow[static_cast<size_t>(*it)];
        }

        const f32 threshold = 420.0f / p_.rivers;  // catchment area (tiles) that sustains a river
        std::vector<i32> channel;
        for (i32 y = 0; y < h_; ++y) {
            for (i32 x = 0; x < w_; ++x) {
                const i32 i = static_cast<i32>(at(x, y));
                if (is_water(i) || flow[static_cast<size_t>(i)] < threshold) continue;
                channel.push_back(i);
                const i32 d = down[static_cast<size_t>(i)];
                if (d >= 0) {
                    // Fill the corner on diagonal steps so the channel is
                    // 4-connected and cannot be slipped through.
                    const i32 xd = d % w_, yd = d / w_;
                    if (xd != x && yd != y) channel.push_back(static_cast<i32>(at(xd, y)));
                }
                // Rivers broaden as tributaries join.
                if (flow[static_cast<size_t>(i)] >= threshold * 5.0f) {
                    if (inside(x + 1, y)) channel.push_back(static_cast<i32>(at(x + 1, y)));
                    if (inside(x, y + 1)) channel.push_back(static_cast<i32>(at(x, y + 1)));
                    if (inside(x + 1, y + 1)) channel.push_back(static_cast<i32>(at(x + 1, y + 1)));
                }
                if (flow[static_cast<size_t>(i)] >= threshold * 16.0f) {
                    if (inside(x - 1, y)) channel.push_back(static_cast<i32>(at(x - 1, y)));
                    if (inside(x, y - 1)) channel.push_back(static_cast<i32>(at(x, y - 1)));
                }
            }
        }
        for (const i32 i : channel) {
            if (is_water(i)) continue;
            terrain(i) = Terrain::Water;
            m_.flags[static_cast<size_t>(i)] |= TileFlag::River;
            ++m_.stats.river_tiles;
        }
    }

    // --- 5. mountains ------------------------------------------------------

    // Thresholding elevation leaves one-tile slivers and specks of rock, which
    // neither block movement meaningfully nor read as mountains. A majority
    // filter consolidates ranges, then anything still tiny is dropped.
    void mountains() {
        rock_.assign(static_cast<size_t>(n_), 0);
        for (i32 i = 0; i < n_; ++i) rock_[static_cast<size_t>(i)] = !is_water(i) && m_.elevation[static_cast<size_t>(i)] >= m_.mountain_level;

        std::vector<u8> next(static_cast<size_t>(n_));
        for (int pass = 0; pass < 2; ++pass) {
            for (i32 y = 0; y < h_; ++y) {
                for (i32 x = 0; x < w_; ++x) {
                    int votes = 0;
                    for (i32 oy = -1; oy <= 1; ++oy) {
                        for (i32 ox = -1; ox <= 1; ++ox) {
                            // Off-map counts as more of the same, so ranges run cleanly off the edge.
                            const i32 nx = clamp(x + ox, 0, w_ - 1), ny = clamp(y + oy, 0, h_ - 1);
                            votes += rock_[at(nx, ny)];
                        }
                    }
                    next[at(x, y)] = votes >= 5 && !is_water(static_cast<i32>(at(x, y)));
                }
            }
            rock_.swap(next);
        }

        const Components ranges = label_components(w_, h_, [&](i32 i) { return rock_[static_cast<size_t>(i)] != 0; });
        for (i32 i = 0; i < n_; ++i) {
            const i32 id = ranges.label[static_cast<size_t>(i)];
            if (id >= 0 && ranges.size[static_cast<size_t>(id)] < 20) rock_[static_cast<size_t>(i)] = 0;
        }
    }

    // --- 6. terrain classification ----------------------------------------

    void classify() {
        // Distance to any water feeds moisture; distance to lakes sizes beaches.
        std::vector<f32> to_water(static_cast<size_t>(n_)), to_lake(static_cast<size_t>(n_));
        for (i32 i = 0; i < n_; ++i) {
            to_water[static_cast<size_t>(i)] = is_water(i) ? 0.0f : kInf;
            to_lake[static_cast<size_t>(i)] = is_water(i) && !is_river(i) ? 0.0f : kInf;
        }
        chamfer_distance(to_water, w_, h_);
        chamfer_distance(to_lake, w_, h_);

        // Ground near water is wetter, so vegetation hugs lakes and rivers.
        for (i32 i = 0; i < n_; ++i) {
            f32& moist = m_.moisture[static_cast<size_t>(i)];
            moist = saturate(moist * 0.8f + 0.3f * std::exp(-to_water[static_cast<size_t>(i)] / 5.0f));
        }

        const u32 s_beach = seed(9);
        std::vector<f32> forest_score(static_cast<size_t>(n_), -kInf);
        std::vector<f32> scores, dryness;
        scores.reserve(static_cast<size_t>(n_));
        dryness.reserve(static_cast<size_t>(n_));

        for (i32 y = 0; y < h_; ++y) {
            for (i32 x = 0; x < w_; ++x) {
                const size_t i = at(x, y);
                if (is_water(static_cast<i32>(i))) continue;
                if (rock_[i]) {
                    m_.terrain[i] = Terrain::Mountain;
                    continue;
                }
                // Beaches vary from nothing to a few tiles wide along the shore.
                if (to_lake[i] < 4.5f) {
                    const f32 width = 3.6f * saturate(0.45f + 0.75f * fbm2(static_cast<f32>(x) / 17.0f, static_cast<f32>(y) / 17.0f, s_beach, 2));
                    if (to_lake[i] <= width) {
                        m_.terrain[i] = Terrain::Sand;
                        continue;
                    }
                }
                // Trees grow in groves (mid-frequency noise) biased to wet ground,
                // and thin out towards the colony so there is room to build.
                const f32 d = start_dist(x, y);
                const f32 clearing = 1.0f - smoothstep(kStartClear, kStartFlat + 6.0f, d);
                forest_score[i] = 0.55f * m_.moisture[i] + 0.45f * (0.5f + 0.5f * grove_[i]) - 0.6f * clearing;
                scores.push_back(forest_score[i]);
                dryness.push_back(m_.moisture[i]);
            }
        }

        const f32 forest_cut = p_.forest > 0.0f ? percentile(scores, 1.0f - p_.forest) : kInf;
        const f32 dry_cut = percentile(dryness, 0.10f);
        for (i32 i = 0; i < n_; ++i) {
            const size_t k = static_cast<size_t>(i);
            if (forest_score[k] == -kInf) continue;
            if (forest_score[k] >= forest_cut) m_.terrain[k] = Terrain::Forest;
            else if (m_.moisture[k] < dry_cut) m_.terrain[k] = Terrain::Dirt;
            else m_.terrain[k] = Terrain::Grass;
        }

        // Scree: bare ground along the foot of mountains.
        for (i32 y = 0; y < h_; ++y) {
            for (i32 x = 0; x < w_; ++x) {
                if (m_.terrain[at(x, y)] != Terrain::Grass || unit(x, y, 10) > 0.6f) continue;
                for (int k = 0; k < 8; ++k) {
                    const i32 nx = x + kDx[k], ny = y + kDy[k];
                    if (inside(nx, ny) && m_.terrain[at(nx, ny)] == Terrain::Mountain) {
                        m_.terrain[at(x, y)] = Terrain::Dirt;
                        break;
                    }
                }
            }
        }
    }

    // --- 7. fords ----------------------------------------------------------

    // Length of the run of river water through (x, y) along (dx, dy), or 0 if
    // the run is too wide or does not end on open ground at both sides.
    i32 crossing_span(i32 x, i32 y, i32 dx, i32 dy, i32& first) const {
        i32 lo = 0, hi = 0;
        const auto river_water = [&](i32 cx, i32 cy) {
            return inside(cx, cy) && is_water(static_cast<i32>(at(cx, cy))) && is_river(static_cast<i32>(at(cx, cy)));
        };
        while (lo > -4 && river_water(x + (lo - 1) * dx, y + (lo - 1) * dy)) --lo;
        while (hi < 4 && river_water(x + (hi + 1) * dx, y + (hi + 1) * dy)) ++hi;
        if (hi - lo + 1 > 3) return 0;
        const i32 ax = x + (lo - 1) * dx, ay = y + (lo - 1) * dy;
        const i32 bx = x + (hi + 1) * dx, by = y + (hi + 1) * dy;
        if (!inside(ax, ay) || !inside(bx, by)) return 0;
        if (!is_open_ground(static_cast<i32>(at(ax, ay))) || !is_open_ground(static_cast<i32>(at(bx, by)))) return 0;
        first = lo;
        return hi - lo + 1;
    }

    // Rivers split the map; fords are the natural crossings (and chokepoints).
    void fords() {
        struct Candidate {
            u32 key;
            i32 x, y;
        };
        std::vector<Candidate> candidates;
        for (i32 y = 2; y < h_ - 2; ++y) {
            for (i32 x = 2; x < w_ - 2; ++x) {
                const i32 i = static_cast<i32>(at(x, y));
                if (is_water(i) && is_river(i)) candidates.push_back({hash2(x, y, seed(11)), x, y});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.key != b.key ? a.key < b.key : (a.y != b.y ? a.y < b.y : a.x < b.x);
        });

        constexpr f32 kSpacing = 20.0f;
        std::vector<IVec2> placed;
        for (const Candidate& c : candidates) {
            bool crowded = false;
            for (const IVec2& f : placed) {
                const f32 dx = static_cast<f32>(f.x - c.x), dy = static_cast<f32>(f.y - c.y);
                if (dx * dx + dy * dy < kSpacing * kSpacing) {
                    crowded = true;
                    break;
                }
            }
            if (crowded) continue;

            // Try to cross along x, then along y.
            for (int axis = 0; axis < 2; ++axis) {
                const i32 dx = axis == 0 ? 1 : 0, dy = 1 - dx;
                i32 first = 0;
                const i32 span = crossing_span(c.x, c.y, dx, dy, first);
                if (span == 0) continue;
                // Two tiles wide: the row through the candidate and the next one.
                for (i32 lane = 0; lane < 2; ++lane) {
                    for (i32 t = first - 1; t <= first + span; ++t) {
                        const i32 tx = c.x + t * dx + lane * dy, ty = c.y + t * dy + lane * dx;
                        if (!inside(tx, ty)) continue;
                        const size_t j = at(tx, ty);
                        if (m_.terrain[j] == Terrain::Water && is_river(static_cast<i32>(j))) m_.terrain[j] = Terrain::Shallows;
                        else if (m_.terrain[j] == Terrain::Forest) m_.terrain[j] = Terrain::Grass;  // keep the banks open
                    }
                }
                placed.push_back({c.x, c.y});
                ++m_.stats.fords;
                break;
            }
        }
    }

    // --- 8. ore deposits ---------------------------------------------------

    void place_deposit(i32 sx, i32 sy, Resource type, Rng& rng) {
        const i32 target = rng.range_i(5, 9);
        std::vector<i32> cluster{static_cast<i32>(at(sx, sy))};
        std::vector<i32> frontier;
        const auto claim = [&](i32 i) {
            m_.terrain[static_cast<size_t>(i)] = Terrain::Dirt;
            m_.resource[static_cast<size_t>(i)] = type;
            m_.flags[static_cast<size_t>(i)] &= static_cast<u8>(~TileFlag::River);
        };
        const auto push_neighbours = [&](i32 i) {
            const i32 x = i % w_, y = i / w_;
            for (int k = 0; k < 4; ++k) {
                const i32 nx = x + kDx[k], ny = y + kDy[k];
                if (!inside(nx, ny) || std::abs(nx - sx) > 2 || std::abs(ny - sy) > 2) continue;
                if (start_dist(nx, ny) < kStartClear + 3.0f) continue;
                const size_t j = at(nx, ny);
                const Terrain t = m_.terrain[j];
                if (t == Terrain::Water || t == Terrain::Shallows || m_.resource[j] != Resource::None) continue;
                frontier.push_back(static_cast<i32>(j));
            }
        };
        claim(cluster[0]);
        push_neighbours(cluster[0]);
        while (static_cast<i32>(cluster.size()) < target && !frontier.empty()) {
            const u32 pick = rng.below(static_cast<u32>(frontier.size()));
            const i32 i = frontier[pick];
            frontier[pick] = frontier.back();
            frontier.pop_back();
            if (m_.resource[static_cast<size_t>(i)] != Resource::None) continue;
            claim(i);
            cluster.push_back(i);
            push_neighbours(i);
        }
        deposit_sites_.push_back({IVec2{sx, sy}, type});
        ++m_.stats.deposits[static_cast<int>(type)];
    }

    bool deposit_within(Resource type, f32 radius) const {
        for (const auto& [pos, t] : deposit_sites_) {
            if (t == type && start_dist(pos.x, pos.y) <= radius) return true;
        }
        return false;
    }

    // Ore crops out where mountains meet open ground. Richer ores lie further
    // from the colony, which pulls the player outwards.
    void deposits() {
        Rng rng(m_.seed ^ 0x0DE9051705ull);
        struct Candidate {
            u32 key;
            i32 x, y;
        };
        std::vector<Candidate> candidates;
        for (i32 y = 1; y < h_ - 1; ++y) {
            for (i32 x = 1; x < w_ - 1; ++x) {
                if (!is_open_ground(static_cast<i32>(at(x, y))) || start_dist(x, y) < kStartFlat) continue;
                bool foot = false;
                for (int k = 0; k < 4 && !foot; ++k) foot = m_.terrain[at(x + kDx[k], y + kDy[k])] == Terrain::Mountain;
                if (foot) candidates.push_back({hash2(x, y, seed(12)), x, y});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.key != b.key ? a.key < b.key : (a.y != b.y ? a.y < b.y : a.x < b.x);
        });

        const i32 wanted = static_cast<i32>(std::lround(static_cast<f32>(n_) / 2100.0f * p_.ore));
        constexpr f32 kSpacing = 15.0f;
        const f32 half = static_cast<f32>(std::min(w_, h_)) * 0.5f;
        for (const Candidate& c : candidates) {
            if (static_cast<i32>(deposit_sites_.size()) >= wanted) break;
            bool crowded = false;
            for (const auto& [pos, t] : deposit_sites_) {
                const f32 dx = static_cast<f32>(pos.x - c.x), dy = static_cast<f32>(pos.y - c.y);
                if (dx * dx + dy * dy < kSpacing * kSpacing) {
                    crowded = true;
                    break;
                }
            }
            if (crowded) continue;
            const f32 far = start_dist(c.x, c.y) / half;
            const f32 roll = rng.next_f32();
            Resource type = roll < 0.50f ? Resource::Stone : (roll < 0.84f ? Resource::Iron : Resource::Gold);
            if (type == Resource::Gold && far < 0.45f) type = Resource::Iron;
            place_deposit(c.x, c.y, type, rng);
        }

        // The colony cannot grow without stone and iron, so guarantee both
        // within reach even when no mountain is nearby.
        ensure_deposit(Resource::Stone, 16.0f, 34.0f, rng);
        ensure_deposit(Resource::Iron, 24.0f, 52.0f, rng);
    }

    void ensure_deposit(Resource type, f32 min_r, f32 max_r, Rng& rng) {
        if (deposit_within(type, max_r)) return;
        IVec2 best{-1, -1};
        u32 best_key = 0xFFFFFFFFu;
        const i32 r = static_cast<i32>(max_r * 0.8f);
        for (i32 y = std::max(3, m_.start.y - r); y <= std::min(h_ - 4, m_.start.y + r); ++y) {
            for (i32 x = std::max(3, m_.start.x - r); x <= std::min(w_ - 4, m_.start.x + r); ++x) {
                const f32 d = start_dist(x, y);
                if (d < min_r || d > max_r * 0.8f) continue;
                bool clear = true;
                for (i32 oy = -2; oy <= 2 && clear; ++oy) {
                    for (i32 ox = -2; ox <= 2 && clear; ++ox) clear = is_open_ground(static_cast<i32>(at(x + ox, y + oy)));
                }
                if (!clear) continue;
                const u32 key = hash2(x, y, seed(13) + static_cast<u32>(type));
                if (key < best_key) {
                    best_key = key;
                    best = {x, y};
                }
            }
        }
        if (best.x >= 0) place_deposit(best.x, best.y, type, rng);
    }

    // --- 9. start clearing -------------------------------------------------

    void clear_start() {
        const i32 r = static_cast<i32>(kStartClear);
        for (i32 y = std::max(0, m_.start.y - r); y <= std::min(h_ - 1, m_.start.y + r); ++y) {
            for (i32 x = std::max(0, m_.start.x - r); x <= std::min(w_ - 1, m_.start.x + r); ++x) {
                if (start_dist(x, y) > kStartClear) continue;
                const size_t i = at(x, y);
                // Dirt and sand are as good to build on as grass; leave them.
                if (m_.terrain[i] != Terrain::Dirt && m_.terrain[i] != Terrain::Sand) m_.terrain[i] = Terrain::Grass;
                m_.resource[i] = Resource::None;
                m_.flags[i] &= static_cast<u8>(~TileFlag::River);
            }
        }
    }

    // --- 10. connectivity ---------------------------------------------------

    // Lakes, rivers, forests and ranges can wall off parts of the map. Any
    // sizeable region the colony cannot reach is joined to it by the cheapest
    // possible cut: a ford through water, a pass through mountains or a gap in
    // the trees. One Dijkstra from the reachable region prices every tile, and
    // each isolated region is linked along its cheapest path back.
    void connect_regions() {
        const Components regions = label_components(w_, h_, [&](i32 i) { return is_walkable(i); });
        const i32 main_region = regions.label[at(m_.start.x, m_.start.y)];
        if (main_region < 0) return;

        const i32 min_size = std::max(48, n_ / 400);
        bool any_isolated = false;
        for (size_t r = 0; r < regions.size.size(); ++r) {
            any_isolated = any_isolated || (static_cast<i32>(r) != main_region && regions.size[r] >= min_size);
        }
        if (!any_isolated) return;

        // Cost in tenths; walkable ground is almost free so paths prefer it.
        const auto cost = [&](i32 i) -> i32 {
            if (is_walkable(i)) return 1;
            if (m_.resource[static_cast<size_t>(i)] != Resource::None) return 80;
            switch (m_.terrain[static_cast<size_t>(i)]) {
                case Terrain::Forest: return 12;
                case Terrain::Water: return is_river(i) ? 40 : 110;
                case Terrain::Mountain: return 70;
                default: return 10;
            }
        };

        // Step costs are small integers, so a ring of buckets (Dial's algorithm)
        // replaces the heap: linear time, no comparisons.
        constexpr i32 kBuckets = 128;  // must exceed the largest step cost
        std::vector<i32> dist(static_cast<size_t>(n_), INT32_MAX);
        std::vector<i32> parent(static_cast<size_t>(n_), -1);
        std::vector<i32> buckets[kBuckets];
        size_t pending = 0;
        for (i32 i = 0; i < n_; ++i) {
            if (regions.label[static_cast<size_t>(i)] == main_region) {
                dist[static_cast<size_t>(i)] = 0;
                buckets[0].push_back(i);
                ++pending;
            }
        }
        for (i32 d = 0; pending > 0; ++d) {
            std::vector<i32>& bucket = buckets[d % kBuckets];
            // The bucket can grow while it is being drained (zero-cost steps do not exist, but equal-distance tiles do).
            for (size_t k = 0; k < bucket.size(); ++k) {
                const i32 i = bucket[k];
                --pending;
                if (dist[static_cast<size_t>(i)] != d) continue;
                const i32 x = i % w_, y = i / w_;
                for (int n = 0; n < 4; ++n) {
                    const i32 nx = x + kDx[n], ny = y + kDy[n];
                    if (!inside(nx, ny)) continue;
                    const i32 j = static_cast<i32>(at(nx, ny));
                    const i32 nd = d + cost(j);
                    if (nd < dist[static_cast<size_t>(j)]) {
                        dist[static_cast<size_t>(j)] = nd;
                        parent[static_cast<size_t>(j)] = i;
                        buckets[nd % kBuckets].push_back(j);
                        ++pending;
                    }
                }
            }
            bucket.clear();
        }

        // Cheapest entry tile of every isolated region.
        std::vector<i32> entry(regions.size.size(), -1);
        for (i32 i = 0; i < n_; ++i) {
            const i32 r = regions.label[static_cast<size_t>(i)];
            if (r < 0 || r == main_region || regions.size[static_cast<size_t>(r)] < min_size) continue;
            i32& e = entry[static_cast<size_t>(r)];
            if (e < 0 || dist[static_cast<size_t>(i)] < dist[static_cast<size_t>(e)]) e = i;
        }

        const auto open_tile = [&](i32 i) {
            if (is_walkable(i)) return;
            const size_t k = static_cast<size_t>(i);
            if (m_.resource[k] != Resource::None) m_.resource[k] = Resource::None;
            switch (m_.terrain[k]) {
                case Terrain::Water: m_.terrain[k] = Terrain::Shallows; break;
                case Terrain::Forest: m_.terrain[k] = Terrain::Grass; break;
                case Terrain::Mountain: m_.terrain[k] = Terrain::Dirt; break;
                default: break;
            }
            ++m_.stats.carved_tiles;
        };
        for (const i32 e : entry) {
            for (i32 i = e; i >= 0 && dist[static_cast<size_t>(i)] > 0; i = parent[static_cast<size_t>(i)]) {
                // Two tiles wide so the link is usable by a crowd.
                const i32 x = i % w_, y = i / w_;
                open_tile(i);
                if (inside(x + 1, y)) open_tile(static_cast<i32>(at(x + 1, y)));
                if (inside(x, y + 1)) open_tile(static_cast<i32>(at(x, y + 1)));
                if (inside(x + 1, y + 1)) open_tile(static_cast<i32>(at(x + 1, y + 1)));
            }
        }
    }

    // --- 11. derived data --------------------------------------------------

    void finish() {
        std::vector<f32> to_land(static_cast<size_t>(n_));
        for (i32 i = 0; i < n_; ++i) to_land[static_cast<size_t>(i)] = is_water(i) ? kInf : 0.0f;
        chamfer_distance(to_land, w_, h_);

        for (i32 i = 0; i < n_; ++i) {
            const size_t k = static_cast<size_t>(i);
            const Terrain t = m_.terrain[k];
            u8 f = m_.flags[k] & TileFlag::River;
            if (t != Terrain::Water && t != Terrain::Shallows) f = 0;
            if (is_walkable(i)) f |= TileFlag::Walkable;
            if ((t == Terrain::Sand || t == Terrain::Grass || t == Terrain::Dirt) && m_.resource[k] == Resource::None) f |= TileFlag::Buildable;
            m_.flags[k] = f;
            m_.shore_dist[k] = static_cast<u8>(std::min(255.0f, to_land[k] * 16.0f));
            ++m_.stats.terrain_tiles[static_cast<int>(t)];
            if (f & TileFlag::Walkable) ++m_.stats.walkable_tiles;
        }

        const Components regions = label_components(w_, h_, [&](i32 i) { return (m_.flags[static_cast<size_t>(i)] & TileFlag::Walkable) != 0; });
        const i32 main_region = regions.label[at(m_.start.x, m_.start.y)];
        if (main_region >= 0) {
            m_.stats.reachable_tiles = regions.size[static_cast<size_t>(main_region)];
            for (i32 y = 0; y < h_; ++y) {
                for (i32 x = 0; x < w_; ++x) {
                    const bool border = x == 0 || y == 0 || x == w_ - 1 || y == h_ - 1;
                    if (border && regions.label[at(x, y)] == main_region) ++m_.stats.reachable_border_tiles;
                }
            }
        }
        const i32 r = static_cast<i32>(kStartFlat);
        for (i32 y = std::max(0, m_.start.y - r); y <= std::min(h_ - 1, m_.start.y + r); ++y) {
            for (i32 x = std::max(0, m_.start.x - r); x <= std::min(w_ - 1, m_.start.x + r); ++x) {
                if (start_dist(x, y) <= kStartFlat && m_.buildable(x, y)) ++m_.stats.buildable_near_start;
            }
        }
    }

    const MapGenParams& p_;
    JobSystem& jobs_;
    TileMap& m_;
    const i32 w_, h_, n_;
    std::vector<f32> grove_;
    std::vector<u8> rock_;
    std::vector<std::pair<IVec2, Resource>> deposit_sites_;
};

}  // namespace

TileMap generate_map(const MapGenParams& params, JobSystem& jobs) {
    ZL_CHECK(params.size >= 64 && params.size <= 4096, "map size must be between 64 and 4096");
    const Stopwatch timer;

    TileMap map;
    map.width = map.height = params.size;
    map.seed = params.seed;
    map.start = {params.size / 2, params.size / 2};
    const size_t n = static_cast<size_t>(params.size) * static_cast<size_t>(params.size);
    map.terrain.assign(n, Terrain::Grass);
    map.resource.assign(n, Resource::None);
    map.flags.assign(n, 0);
    map.elevation.assign(n, 0.0f);
    map.moisture.assign(n, 0.0f);
    map.shore_dist.assign(n, 0);

    Generator(params, jobs, map).run();
    map.stats.gen_ms = timer.millis();
    return map;
}

}  // namespace zl
