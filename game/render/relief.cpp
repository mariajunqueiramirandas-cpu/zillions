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

// Height, in tiles, that one unit of map elevation rises in open country.
constexpr f32 kPlainHeight = 16.0f;

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
    const f32 texel = 1.0f / static_cast<f32>(kReliefScale);  // tiles per texel

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
    const Grid g{w * kReliefScale, h * kReliefScale};
    const size_t n = static_cast<size_t>(g.w) * static_cast<size_t>(g.h);

    // Tile heights for open country. Mountains are held at the level of their
    // foot here; their shape is added by erosion below.
    const f32 foot = (map.mountain_level - map.sea_level) * kPlainHeight;
    std::vector<f32> coarse(static_cast<size_t>(w * h));
    for (i32 i = 0; i < w * h; ++i) {
        const size_t k = static_cast<size_t>(i);
        const f32 e = clamp(map.elevation[k], map.sea_level, map.mountain_level);
        coarse[k] = map.terrain[k] == Terrain::Water ? 0.0f : (e - map.sea_level) * kPlainHeight;
    }
    const auto tile_height = [&](i32 x, i32 y) { return coarse[static_cast<size_t>(clamp(y, 0, h - 1) * w + clamp(x, 0, w - 1))]; };

    // Spline upsampling keeps slopes continuous across tile borders.
    std::vector<f32> height(n);
    std::vector<u8> rock(n);
    jobs.parallel_for(g.h, 16, [&](i32 y0, i32 y1) {
        for (i32 y = y0; y < y1; ++y) {
            const f32 ty = (static_cast<f32>(y) + 0.5f) / kReliefScale - 0.5f;
            const i32 iy = static_cast<i32>(std::floor(ty));
            const f32 fy = ty - static_cast<f32>(iy);
            for (i32 x = 0; x < g.w; ++x) {
                const f32 tx = (static_cast<f32>(x) + 0.5f) / kReliefScale - 0.5f;
                const i32 ix = static_cast<i32>(std::floor(tx));
                const f32 fx = tx - static_cast<f32>(ix);
                f32 rows[4];
                for (i32 r = 0; r < 4; ++r) {
                    rows[r] = catmull_rom(tile_height(ix - 1, iy - 1 + r), tile_height(ix, iy - 1 + r), tile_height(ix + 1, iy - 1 + r), tile_height(ix + 2, iy - 1 + r), fx);
                }
                height[g.at(x, y)] = std::max(0.0f, catmull_rom(rows[0], rows[1], rows[2], rows[3], fy));
                rock[g.at(x, y)] = map.terrain[static_cast<size_t>(map.index(x / kReliefScale, y / kReliefScale))] == Terrain::Mountain;
            }
        }
    });

    std::vector<f32> rise(n, 0.0f);
    erode_mountains(g, rock, static_cast<u32>(mix64(map.seed ^ 0xC4A6ull)), jobs, rise);
    for (size_t i = 0; i < n; ++i) height[i] += rise[i];
    {
        // A light blur rounds off the stair-steps of eight-direction drainage.
        std::vector<f32> soft(n);
        box_blur(g, height, soft, 1, jobs);
        height.swap(soft);
    }

    // Cavity: height relative to the neighbourhood average. Positive on ridges
    // and negative in valleys; the shader uses it to pick both out.
    std::vector<f32> blurred(n);
    box_blur(g, height, blurred, kReliefScale + 1, jobs);

    f32 top = 1.0e-3f;
    for (const f32 v : height) top = std::max(top, v);

    Relief relief;
    relief.texture = Image(g.w, g.h);
    relief.height_range = top;
    relief.mountain_foot = foot / top;
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
                const f32 per_tile = static_cast<f32>(kReliefScale) / 8.0f;
                const f32 nx = -gx * per_tile, ny = -gy * per_tile;
                const f32 inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
                const size_t i = g.at(x, y);
                const auto signed_byte = [](f32 v) { return static_cast<u8>(clamp(v * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                const auto unit_byte = [](f32 v) { return static_cast<u8>(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                relief.texture.at(x, y) = {signed_byte(nx * inv), signed_byte(ny * inv), unit_byte(height[i] / top), signed_byte((height[i] - blurred[i]) * 1.6f)};
            }
        }
    });
    return relief;
}

}  // namespace zl
