// Seeded gradient noise. Lattice gradients come from an integer hash rather
// than a permutation table, so there is no shared state: every function here is
// pure and can be evaluated from any thread.
#pragma once

#include <cmath>

#include "core/base.h"
#include "core/rng.h"

namespace zl {

namespace noise_detail {

// 16 unit vectors evenly spaced around the circle.
inline constexpr f32 kGrad[16][2] = {
    {1.000000f, 0.000000f},   {0.923880f, 0.382683f},   {0.707107f, 0.707107f},   {0.382683f, 0.923880f},
    {0.000000f, 1.000000f},   {-0.382683f, 0.923880f},  {-0.707107f, 0.707107f},  {-0.923880f, 0.382683f},
    {-1.000000f, 0.000000f},  {-0.923880f, -0.382683f}, {-0.707107f, -0.707107f}, {-0.382683f, -0.923880f},
    {0.000000f, -1.000000f},  {0.382683f, -0.923880f},  {0.707107f, -0.707107f},  {0.923880f, -0.382683f},
};

// floor() without the libm call that baseline x86-64 would otherwise make.
ZL_FORCE_INLINE i32 fast_floor(f32 v) {
    const i32 i = static_cast<i32>(v);
    return i - static_cast<i32>(v < static_cast<f32>(i));
}

// Lattice hash for gradient selection: cheaper than hash2, and the high bits
// (the only ones used) are well mixed.
ZL_FORCE_INLINE u32 lattice_hash(i32 x, i32 y, u32 seed) {
    u32 h = seed ^ (static_cast<u32>(x) * 0x9E3779B1u) ^ (static_cast<u32>(y) * 0x85EBCA77u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    return h;
}

ZL_FORCE_INLINE f32 corner(i32 ix, i32 iy, f32 dx, f32 dy, u32 seed) {
    f32 t = 0.5f - dx * dx - dy * dy;
    if (t <= 0.0f) return 0.0f;
    const f32* g = kGrad[lattice_hash(ix, iy, seed) >> 28];
    t *= t;
    return t * t * (g[0] * dx + g[1] * dy);
}

}  // namespace noise_detail

// 2D simplex noise, output in roughly [-1, 1].
inline f32 simplex2(f32 x, f32 y, u32 seed) {
    constexpr f32 kSkew = 0.3660254037844386f;    // (sqrt(3) - 1) / 2
    constexpr f32 kUnskew = 0.21132486540518713f; // (3 - sqrt(3)) / 6

    const f32 s = (x + y) * kSkew;
    const i32 i = noise_detail::fast_floor(x + s);
    const i32 j = noise_detail::fast_floor(y + s);
    const f32 fi = static_cast<f32>(i);
    const f32 fj = static_cast<f32>(j);

    const f32 t = (fi + fj) * kUnskew;
    const f32 x0 = x - (fi - t);
    const f32 y0 = y - (fj - t);

    const i32 i1 = x0 > y0 ? 1 : 0;
    const i32 j1 = 1 - i1;

    const f32 x1 = x0 - static_cast<f32>(i1) + kUnskew;
    const f32 y1 = y0 - static_cast<f32>(j1) + kUnskew;
    const f32 x2 = x0 - 1.0f + 2.0f * kUnskew;
    const f32 y2 = y0 - 1.0f + 2.0f * kUnskew;

    const f32 n = noise_detail::corner(i, j, x0, y0, seed) +
                  noise_detail::corner(i + i1, j + j1, x1, y1, seed) +
                  noise_detail::corner(i + 1, j + 1, x2, y2, seed);
    return n * 99.2f;
}

// Fractal Brownian motion: summed octaves, normalised to roughly [-1, 1].
inline f32 fbm2(f32 x, f32 y, u32 seed, int octaves, f32 lacunarity = 2.0f, f32 gain = 0.5f) {
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * simplex2(x, y, seed + static_cast<u32>(o) * 0x632BE5ABu);
        norm += amp;
        amp *= gain;
        x *= lacunarity;
        y *= lacunarity;
    }
    return sum / norm;
}

// Ridged multifractal: sharp crests where the underlying noise crosses zero.
// Output in [0, 1]; good for mountain ranges.
inline f32 ridged2(f32 x, f32 y, u32 seed, int octaves, f32 lacunarity = 2.0f, f32 gain = 0.5f) {
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f, weight = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        f32 n = 1.0f - std::fabs(simplex2(x, y, seed + static_cast<u32>(o) * 0x632BE5ABu));
        n *= n;
        sum += amp * n * weight;
        norm += amp;
        // Later octaves only add detail where earlier ones already formed a ridge.
        weight = n > 1.0f ? 1.0f : n;
        amp *= gain;
        x *= lacunarity;
        y *= lacunarity;
    }
    return sum / norm;
}

}  // namespace zl
