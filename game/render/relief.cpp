#include "render/relief.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "core/jobs.h"
#include "core/rng.h"
#include "math/noise.h"
#include "math/vec.h"

namespace zl {

namespace {

constexpr i32 kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr i32 kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr f32 kDiag = 1.41421356f;

// Height, in world units, that one unit of map elevation rises within a level: a
// faint roll that keeps flat ground from looking dead.
constexpr f32 kPlainHeight = 6.0f;
// Height of one level above the next, in world units.
constexpr f32 kLevelHeight = 1.6f;
// Half-width of a cliff face, as a fraction of the blend between two levels.
constexpr f32 kCliffWidth = 0.20f;

// Mountain erosion model (see erode_mountains).
constexpr int kErosionPasses = 3;
constexpr f32 kRidgeSlope = 1.15f;     // steepest slope, at a ridge crest (rise per tile)
constexpr f32 kConcavity = 0.42f;      // how quickly slopes flatten as drainage grows
constexpr f32 kHillslopeArea = 28.0f;  // drainage (texels) needed to cut a channel
constexpr f32 kRillConcavity = 0.11f;   // faint concavity of the flanks themselves
constexpr f32 kValleySlope = 0.035f;   // floor on the slope factor, so valleys still fall
constexpr f32 kRoutingNoise = 4.0f;    // texels of noise on the initial drainage surface

f32 catmull_rom(f32 a, f32 b, f32 c, f32 d, f32 t) {
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}

struct Grid {
    i32 w, h;
    bool inside(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    size_t at(i32 x, i32 y) const { return static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x); }
};

// Carves the mountains. Real ranges are shaped by water: slopes are steep where
// little water gathers (crests) and gentle where a lot does (valley floors).
// This reproduces that directly instead of simulating rainfall over time:
//   1. route drainage from every mountain texel out to the foot of the range;
//   2. count the area draining through each texel;
//   3. rebuild heights from the foot upwards, each texel standing above the
//      one it drains to by a slope that shrinks with drainage area (the
//      stream-power law at equilibrium: slope ~ area^-concavity).
// Channels become valleys, the divides between them become sharp ridges, and
// because drainage is a tree the ridges branch into spurs. Repeating the three
// steps on the new surface lets divides settle where both flanks agree.
//
// `rock` marks mountain texels; on return `rise` holds height above the foot.
void erode_mountains(const Grid& g, const std::vector<u8>& rock, u32 seed, JobSystem& jobs, std::vector<f32>& rise) {
    const size_t n = static_cast<size_t>(g.w) * static_cast<size_t>(g.h);
    const f32 texel = 1.0f / static_cast<f32>(kReliefTexelsPerUnit);  // world units per texel

    // Distance to the foot of the range, in texels: the first drainage surface
    // is a dome, so the first main divide follows the middle of each massif.
    std::vector<f32> surface(n);
    for (size_t i = 0; i < n; ++i) surface[i] = rock[i] ? 1.0e9f : 0.0f;
    for (i32 y = 0; y < g.h; ++y) {
        for (i32 x = 0; x < g.w; ++x) {
            const size_t i = g.at(x, y);
            if (!rock[i]) continue;
            f32 v = surface[i];
            if (x > 0) v = std::min(v, surface[i - 1] + 1.0f);
            if (y > 0) {
                v = std::min(v, surface[i - g.w] + 1.0f);
                if (x > 0) v = std::min(v, surface[i - g.w - 1] + kDiag);
                if (x < g.w - 1) v = std::min(v, surface[i - g.w + 1] + kDiag);
            }
            surface[i] = v;
        }
    }
    for (i32 y = g.h - 1; y >= 0; --y) {
        for (i32 x = g.w - 1; x >= 0; --x) {
            const size_t i = g.at(x, y);
            if (!rock[i]) continue;
            f32 v = surface[i];
            if (x < g.w - 1) v = std::min(v, surface[i + 1] + 1.0f);
            if (y < g.h - 1) {
                v = std::min(v, surface[i + g.w] + 1.0f);
                if (x > 0) v = std::min(v, surface[i + g.w - 1] + kDiag);
                if (x < g.w - 1) v = std::min(v, surface[i + g.w + 1] + kDiag);
            }
            surface[i] = v;
        }
    }

    // Noise makes channels wander and merge; uplift varies the height along a
    // range so it has distinct peaks and saddles.
    std::vector<f32> uplift(n, 1.0f);
    jobs.parallel_for(g.h, 16, [&](i32 y0, i32 y1) {
        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = 0; x < g.w; ++x) {
                const size_t i = g.at(x, y);
                if (!rock[i]) continue;
                const f32 tx = static_cast<f32>(x) * texel, ty = static_cast<f32>(y) * texel;
                surface[i] += kRoutingNoise * fbm2(tx / 2.6f, ty / 2.6f, seed, 3);
                uplift[i] = 0.62f + 0.76f * (0.5f + 0.5f * fbm2(tx / 8.0f, ty / 8.0f, seed + 1, 2));
            }
        }
    });

    // The texels to process, and those on the foot of a range (touching open
    // ground), where drainage leaves the mountains. Both are fixed across passes.
    std::vector<i32> texels, foot;
    for (i32 y = 0; y < g.h; ++y) {
        for (i32 x = 0; x < g.w; ++x) {
            const size_t i = g.at(x, y);
            if (!rock[i]) continue;
            texels.push_back(static_cast<i32>(i));
            for (int k = 0; k < 8; ++k) {
                if (g.inside(x + kDx[k], y + kDy[k]) && !rock[g.at(x + kDx[k], y + kDy[k])]) {
                    foot.push_back(static_cast<i32>(i));
                    break;
                }
            }
        }
    }

    std::vector<i32> down(n), order;
    std::vector<u8> closed(n, 1);
    std::vector<f32> area(n), level(n);
    order.reserve(texels.size());
    using Node = std::pair<f32, i32>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    for (int pass = 0; pass < kErosionPasses; ++pass) {
        // --- 1. drainage: priority flood inwards from the foot of the range ---
        order.clear();
        for (const i32 i : texels) {
            closed[static_cast<size_t>(i)] = 0;
            level[static_cast<size_t>(i)] = surface[static_cast<size_t>(i)];
        }
        for (const i32 i : foot) {
            closed[static_cast<size_t>(i)] = 1;
            down[static_cast<size_t>(i)] = -1;
            open.push({level[static_cast<size_t>(i)], i});
            order.push_back(i);
        }
        while (!open.empty()) {
            const auto [lv, i] = open.top();
            open.pop();
            const i32 x = i % g.w, y = i / g.w;
            for (int k = 0; k < 8; ++k) {
                const i32 nx = x + kDx[k], ny = y + kDy[k];
                if (!g.inside(nx, ny)) continue;
                const size_t j = g.at(nx, ny);
                if (closed[j]) continue;
                closed[j] = 1;
                down[j] = i;
                level[j] = std::max(level[j], lv);
                open.push({level[j], static_cast<i32>(j)});
                order.push_back(static_cast<i32>(j));
            }
        }
        // Rock with no route to open ground (a range filling the whole map) stays flat.
        for (const i32 i : texels) closed[static_cast<size_t>(i)] = 1;

        // --- 2. drainage area: children are always listed after their parent ---
        for (const i32 i : order) area[static_cast<size_t>(i)] = 1.0f;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const i32 d = down[static_cast<size_t>(*it)];
            if (d >= 0) area[static_cast<size_t>(d)] += area[static_cast<size_t>(*it)];
        }

        // --- 3. heights from the foot upwards ---
        for (const i32 i : order) {
            const size_t k = static_cast<size_t>(i);
            const i32 d = down[k];
            f32 below = 0.0f, run = texel;
            if (d >= 0) {
                below = rise[static_cast<size_t>(d)];
                if (d % g.w != i % g.w && d / g.w != i / g.w) run *= kDiag;
            }
            // Below the hillslope area no channel forms: flanks keep a nearly
            // even slope and read as broad faces, with only faint rills.
            const f32 relative = area[k] / kHillslopeArea;
            const f32 gentle = relative <= 1.0f ? std::pow(relative, -kRillConcavity) : std::max(std::pow(relative, -kConcavity), kValleySlope);
            rise[k] = below + run * kRidgeSlope * uplift[k] * gentle;
        }

        // The next pass drains the surface this one produced.
        if (pass + 1 < kErosionPasses) {
            for (const i32 i : order) {
                const size_t k = static_cast<size_t>(i);
                surface[k] = rise[k] / texel + 0.02f * hash_to_unit(hash2(i % g.w, i / g.w, seed + 7u + static_cast<u32>(pass)));
            }
        }
    }

    // A divide whose flanks disagree leaves a wall. Cap the slope everywhere by
    // taking the lower envelope of cones (a two-sweep distance transform seeded
    // with the heights), which only trims those walls.
    const f32 cap = kRidgeSlope * 1.45f * texel, cap_diag = cap * kDiag;
    const auto h = [&](i32 x, i32 y) { return rise[g.at(x, y)]; };
    for (i32 y = 0; y < g.h; ++y) {
        for (i32 x = 0; x < g.w; ++x) {
            const size_t i = g.at(x, y);
            if (!rock[i]) continue;
            f32 v = rise[i];
            if (x > 0) v = std::min(v, h(x - 1, y) + cap);
            if (y > 0) {
                v = std::min(v, h(x, y - 1) + cap);
                if (x > 0) v = std::min(v, h(x - 1, y - 1) + cap_diag);
                if (x < g.w - 1) v = std::min(v, h(x + 1, y - 1) + cap_diag);
            }
            rise[i] = v;
        }
    }
    for (i32 y = g.h - 1; y >= 0; --y) {
        for (i32 x = g.w - 1; x >= 0; --x) {
            const size_t i = g.at(x, y);
            if (!rock[i]) continue;
            f32 v = rise[i];
            if (x < g.w - 1) v = std::min(v, h(x + 1, y) + cap);
            if (y < g.h - 1) {
                v = std::min(v, h(x, y + 1) + cap);
                if (x > 0) v = std::min(v, h(x - 1, y + 1) + cap_diag);
                if (x < g.w - 1) v = std::min(v, h(x + 1, y + 1) + cap_diag);
            }
            rise[i] = v;
        }
    }
}

// Separable box blur with clamped edges; radius in texels.
void box_blur(const Grid& g, const std::vector<f32>& src, std::vector<f32>& dst, i32 radius, JobSystem& jobs) {
    std::vector<f32> tmp(src.size());
    const f32 norm = 1.0f / static_cast<f32>(2 * radius + 1);
    jobs.parallel_for(g.h, 32, [&](i32 y0, i32 y1) {
        for (i32 y = y0; y < y1; ++y) {
            const f32* row = src.data() + g.at(0, y);
            f32 sum = 0.0f;
            for (i32 k = -radius; k <= radius; ++k) sum += row[clamp(k, 0, g.w - 1)];
            for (i32 x = 0; x < g.w; ++x) {
                tmp[g.at(x, y)] = sum * norm;
                sum += row[std::min(x + radius + 1, g.w - 1)] - row[std::max(x - radius, 0)];
            }
        }
    });
    jobs.parallel_for(g.w, 32, [&](i32 x0, i32 x1) {
        for (i32 x = x0; x < x1; ++x) {
            f32 sum = 0.0f;
            for (i32 k = -radius; k <= radius; ++k) sum += tmp[g.at(x, clamp(k, 0, g.h - 1))];
            for (i32 y = 0; y < g.h; ++y) {
                dst[g.at(x, y)] = sum * norm;
                sum += tmp[g.at(x, std::min(y + radius + 1, g.h - 1))] - tmp[g.at(x, std::max(y - radius, 0))];
            }
        }
    });
}

}  // namespace

Relief build_relief(const TileMap& map, JobSystem& jobs) {
    const i32 w = map.width, h = map.height;
    // Texels per tile. The relief is laid out in world units, so a finer tile
    // grid over the same geography does not need a larger relief.
    const i32 scale = std::max(1, kReliefTexelsPerUnit / map.resolution);
    const Grid g{w * scale, h * scale};
    const size_t n = static_cast<size_t>(g.w) * static_cast<size_t>(g.h);
    const size_t tiles = static_cast<size_t>(w) * static_cast<size_t>(h);
    const auto tile_at = [&](i32 x, i32 y) { return static_cast<size_t>(clamp(y, 0, h - 1) * w + clamp(x, 0, w - 1)); };

    // Per tile: gentle undulation within a level, and the level itself as a
    // number the texels can interpolate. Ramp tiles sit part-way between the
    // levels they join. Under mountains the level is relaxed into a smooth
    // surface, so a range standing across a cliff line has no step inside it.
    std::vector<f32> undulation(tiles), step(tiles);
    std::vector<u8> smooth(tiles);  // 1 where the surface slopes instead of breaking into a cliff
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const size_t k = tile_at(x, y);
            const bool is_rock = map.terrain[k] == Terrain::Mountain, is_ramp = (map.flags[k] & TileFlag::Ramp) != 0;
            const f32 e = clamp(map.elevation[k], map.sea_level, map.mountain_level);
            undulation[k] = map.terrain[k] == Terrain::Water ? 0.0f : (e - map.sea_level) * kPlainHeight;
            step[k] = static_cast<f32>(map.level[k]);
            if (is_ramp) {
                for (int d = 0; d < 4; ++d) {
                    const size_t j = tile_at(x + kDx[d], y + kDy[d]);
                    if (map.terrain[j] == Terrain::Mountain) continue;
                    if (map.level[j] > map.level[k]) step[k] = static_cast<f32>(map.level[k]) + 0.3f;
                    if (map.level[j] < map.level[k]) step[k] = static_cast<f32>(map.level[k]) - 0.3f;
                }
            }
            smooth[k] = is_rock || is_ramp;
        }
    }
    for (int pass = 0; pass < 32; ++pass) {
        for (i32 y = 0; y < h; ++y) {
            for (i32 x = 0; x < w; ++x) {
                const size_t k = tile_at(x, y);
                if (map.terrain[k] != Terrain::Mountain) continue;
                step[k] = 0.25f * (step[tile_at(x - 1, y)] + step[tile_at(x + 1, y)] + step[tile_at(x, y - 1)] + step[tile_at(x, y + 1)]);
            }
        }
    }
    const u32 seed = static_cast<u32>(mix64(map.seed ^ 0xC4A6ull));
    std::vector<f32> height(n), terrace(n), gentle(n), cliff(n);
    std::vector<u8> rock(n);
    jobs.parallel_for(g.h, 16, [&](i32 y0, i32 y1) {
        for (i32 y = y0; y < y1; ++y) {
            const f32 ty = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(scale) - 0.5f;  // in tiles, relative to tile centres
            const i32 iy = static_cast<i32>(std::floor(ty));
            const f32 fy = ty - static_cast<f32>(iy);
            for (i32 x = 0; x < g.w; ++x) {
                const f32 tx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(scale) - 0.5f;
                const i32 ix = static_cast<i32>(std::floor(tx));
                const f32 fx = tx - static_cast<f32>(ix);
                const size_t i = g.at(x, y);
                rock[i] = map.terrain[tile_at(x / scale, y / scale)] == Terrain::Mountain;

                // Undulation: spline upsampling keeps slopes continuous across tile borders.
                f32 rows[4];
                for (i32 r = 0; r < 4; ++r) {
                    rows[r] = catmull_rom(undulation[tile_at(ix - 1, iy - 1 + r)], undulation[tile_at(ix, iy - 1 + r)],
                                          undulation[tile_at(ix + 1, iy - 1 + r)], undulation[tile_at(ix + 2, iy - 1 + r)], fx);
                }
                height[i] = std::max(0.0f, catmull_rom(rows[0], rows[1], rows[2], rows[3], fy));

                // Level and "no cliff here" mask, interpolated between tile centres.
                const size_t k00 = tile_at(ix, iy), k10 = tile_at(ix + 1, iy), k01 = tile_at(ix, iy + 1), k11 = tile_at(ix + 1, iy + 1);
                const f32 w00 = (1.0f - fx) * (1.0f - fy), w10 = fx * (1.0f - fy), w01 = (1.0f - fx) * fy, w11 = fx * fy;
                terrace[i] = w00 * step[k00] + w10 * step[k10] + w01 * step[k01] + w11 * step[k11];
                gentle[i] = w00 * smooth[k00] + w10 * smooth[k10] + w01 * smooth[k01] + w11 * smooth[k11];
            }
        }
    });

    // Blurring the level field turns the staircase outline of each terrace
    // into a smooth curve; the cliff is then drawn where that curve crosses the
    // half-way mark between two levels.
    {
        std::vector<f32> tmp(n);
        // Two wide passes approximate a Gaussian about a tile across, enough
        // to iron out the one-tile ripple of a diagonal staircase.
        box_blur(g, terrace, tmp, kReliefTexelsPerUnit, jobs);
        box_blur(g, tmp, terrace, kReliefTexelsPerUnit, jobs);
        box_blur(g, gentle, tmp, kReliefTexelsPerUnit / 2, jobs);
        gentle.swap(tmp);
    }
    jobs.parallel_for(g.h, 16, [&](i32 y0, i32 y1) {
        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = 0; x < g.w; ++x) {
                const size_t i = g.at(x, y);
                const f32 px = (static_cast<f32>(x) + 0.5f) / kReliefTexelsPerUnit, py = (static_cast<f32>(y) + 0.5f) / kReliefTexelsPerUnit;  // world units
                const f32 ease = smoothstep(0.2f, 0.7f, gentle[i]);
                // A little noise makes the cliff line wander. It moves the line
                // only where levels are changing; flat ground snaps back to whole levels.
                const f32 value = terrace[i] + 0.11f * simplex2(px / 2.1f, py / 2.1f, seed + 21u) * (1.0f - ease);
                const f32 whole = std::floor(value), part = value - whole;
                const f32 snapped = whole + smoothstep(0.5f - kCliffWidth, 0.5f + kCliffWidth, part);
                terrace[i] = lerp(snapped, terrace[i], ease);
                cliff[i] = (1.0f - ease) * (1.0f - smoothstep(kCliffWidth * 0.75f, kCliffWidth * 1.5f, std::fabs(part - 0.5f)));
            }
        }
    });

    std::vector<f32> rise(n, 0.0f);
    erode_mountains(g, rock, seed, jobs, rise);
    f32 highest_rise = 1.0e-3f;
    for (size_t i = 0; i < n; ++i) {
        height[i] += terrace[i] * kLevelHeight + rise[i];
        highest_rise = std::max(highest_rise, rise[i]);
    }
    {
        // A light blur rounds off the stair-steps of eight-direction drainage.
        std::vector<f32> soft(n);
        box_blur(g, height, soft, 1, jobs);
        height.swap(soft);
    }

    // Cavity: height relative to the neighbourhood average. Positive on ridges
    // and negative in valleys; the shader uses it to pick both out.
    std::vector<f32> blurred(n);
    box_blur(g, height, blurred, kReliefTexelsPerUnit + 1, jobs);

    f32 top = 1.0e-3f;
    for (const f32 v : height) top = std::max(top, v);

    Relief relief;
    relief.shape = Image(g.w, g.h);
    relief.surface = Image(g.w, g.h);
    relief.height_range = top;
    const auto sample = [&](i32 x, i32 y) { return height[g.at(clamp(x, 0, g.w - 1), clamp(y, 0, g.h - 1))]; };
    jobs.parallel_for(g.h, 16, [&](i32 y0, i32 y1) {
        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = 0; x < g.w; ++x) {
                // Sobel gradient: smoother than plain central differences.
                const f32 gx = (sample(x + 1, y - 1) + 2.0f * sample(x + 1, y) + sample(x + 1, y + 1)) -
                               (sample(x - 1, y - 1) + 2.0f * sample(x - 1, y) + sample(x - 1, y + 1));
                const f32 gy = (sample(x - 1, y + 1) + 2.0f * sample(x, y + 1) + sample(x + 1, y + 1)) -
                               (sample(x - 1, y - 1) + 2.0f * sample(x, y - 1) + sample(x + 1, y - 1));
                // The kernel spans two texels and sums to 4 per unit slope.
                const f32 per_unit = static_cast<f32>(kReliefTexelsPerUnit) / 8.0f;
                const f32 nx = -gx * per_unit, ny = -gy * per_unit;
                const f32 inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
                const size_t i = g.at(x, y);
                const auto signed_byte = [](f32 v) { return static_cast<u8>(clamp(v * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                const auto unit_byte = [](f32 v) { return static_cast<u8>(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                relief.shape.at(x, y) = {signed_byte(nx * inv), signed_byte(ny * inv), unit_byte(height[i] / top), signed_byte((height[i] - blurred[i]) * 1.6f)};
                relief.surface.at(x, y) = {unit_byte(rise[i] / highest_rise), unit_byte(cliff[i]), unit_byte(terrace[i] / static_cast<f32>(kHeightLevels - 1)), 255};
            }
        }
    });
    return relief;
}

}  // namespace zl
