// Deterministic hashing and random numbers. Everything in the simulation that
// needs randomness goes through here so that a seed fully determines a game.
#pragma once

#include "core/base.h"

namespace zl {

// splitmix64 finaliser: a fast, high quality 64-bit mixer.
constexpr u64 mix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Low-bias 32-bit integer hash (Wellons' "lowbias32").
constexpr u32 hash32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// Stateless hash of a 2D lattice point; used for per-tile randomness that must
// not depend on evaluation order (and therefore is safe to use from any thread).
constexpr u32 hash2(i32 x, i32 y, u32 seed) {
    return hash32(static_cast<u32>(x) * 0x9E3779B1u ^ hash32(static_cast<u32>(y) + seed));
}

// Maps a hash to [0, 1).
constexpr f32 hash_to_unit(u32 h) { return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); }

// PCG32 (O'Neill): small state, good statistical quality, fast.
class Rng {
public:
    explicit constexpr Rng(u64 seed = 0x853C49E6748FEA9Bull) { reseed(seed); }

    constexpr void reseed(u64 seed) {
        state_ = 0;
        inc_ = (mix64(seed) << 1) | 1u;
        next_u32();
        state_ += mix64(seed ^ 0xDA3E39CB94B95BDBull);
        next_u32();
    }

    constexpr u32 next_u32() {
        const u64 old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        const u32 xorshifted = static_cast<u32>(((old >> 18) ^ old) >> 27);
        const u32 rot = static_cast<u32>(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
    }

    // Uniform in [0, 1).
    constexpr f32 next_f32() { return hash_to_unit(next_u32()); }

    // Uniform in [lo, hi).
    constexpr f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * next_f32(); }

    // Uniform integer in [0, n). Uses Lemire's multiply-shift reduction.
    constexpr u32 below(u32 n) { return static_cast<u32>((static_cast<u64>(next_u32()) * n) >> 32); }

    // Uniform integer in [lo, hi] (inclusive).
    constexpr i32 range_i(i32 lo, i32 hi) { return lo + static_cast<i32>(below(static_cast<u32>(hi - lo + 1))); }

    constexpr bool chance(f32 p) { return next_f32() < p; }

private:
    u64 state_ = 0;
    u64 inc_ = 1;
};

}  // namespace zl
