#include "render/terrain_art.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/jobs.h"
#include "core/rng.h"
#include "math/vec.h"

namespace zl {

namespace {

constexpr f32 kPi = 3.14159265f;

struct Rgb {
    f32 r, g, b;
};
struct Rgba {
    f32 r, g, b, a;  // straight (non-premultiplied) alpha, components in 0..1
};

constexpr Rgb rgb(int r, int g, int b) { return {static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f, static_cast<f32>(b) / 255.0f}; }
constexpr Rgb mix(Rgb a, Rgb b, f32 t) { return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t)}; }
constexpr Rgb scale(Rgb c, f32 s) { return {c.r * s, c.g * s, c.b * s}; }
constexpr Rgba opaque(Rgb c, f32 a = 1.0f) { return {c.r, c.g, c.b, a}; }
constexpr Rgba kClear{0, 0, 0, 0};

u8 to_byte(f32 v) { return static_cast<u8>(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
Color to_color(Rgb c) { return {to_byte(c.r), to_byte(c.g), to_byte(c.b), 255}; }

// Light comes from the upper left, slightly towards the viewer.
constexpr f32 kLightX = -0.50f, kLightY = -0.62f, kLightZ = 0.60f;

// --- noise for texture synthesis -------------------------------------------

f32 fade(f32 t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// Value noise that repeats every `period` lattice cells, so textures tile.
f32 periodic_noise(f32 x, f32 y, i32 period, u32 seed) {
    const f32 fx = std::floor(x), fy = std::floor(y);
    const i32 xi = static_cast<i32>(fx), yi = static_cast<i32>(fy);
    const auto wrap = [period](i32 v) { return ((v % period) + period) % period; };
    const i32 x0 = wrap(xi), x1 = wrap(xi + 1), y0 = wrap(yi), y1 = wrap(yi + 1);
    const f32 u = fade(x - fx), v = fade(y - fy);
    const f32 a = hash_to_unit(hash2(x0, y0, seed)), b = hash_to_unit(hash2(x1, y0, seed));
    const f32 c = hash_to_unit(hash2(x0, y1, seed)), d = hash_to_unit(hash2(x1, y1, seed));
    return lerp(lerp(a, b, u), lerp(c, d, u), v);
}

// Tileable fBm over the unit square; `period` is the lattice size of the first octave.
f32 periodic_fbm(f32 u, f32 v, i32 period, int octaves, u32 seed) {
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * periodic_noise(u * static_cast<f32>(period), v * static_cast<f32>(period), period, seed + static_cast<u32>(o) * 131u);
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / norm;
}

f32 periodic_ridged(f32 u, f32 v, i32 period, int octaves, u32 seed) {
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const f32 n = periodic_noise(u * static_cast<f32>(period), v * static_cast<f32>(period), period, seed + static_cast<u32>(o) * 131u);
        const f32 r = 1.0f - std::fabs(2.0f * n - 1.0f);
        sum += amp * r * r;
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / norm;
}

// Non-periodic value noise for sprites.
f32 value_noise(f32 x, f32 y, u32 seed) { return periodic_noise(x, y, 1 << 20, seed); }

// --- ground textures --------------------------------------------------------

// Sparse pebbles on a wrapping grid. Returns a brightness offset (0 if none).
f32 pebbles(i32 px, i32 py, i32 size, i32 cell, f32 density, f32 max_radius, u32 seed) {
    const i32 cells = size / cell;
    f32 result = 0.0f;
    for (i32 oy = -1; oy <= 1; ++oy) {
        for (i32 ox = -1; ox <= 1; ++ox) {
            const i32 cx = ((px / cell + ox) % cells + cells) % cells, cy = ((py / cell + oy) % cells + cells) % cells;
            const u32 h = hash2(cx, cy, seed);
            if (hash_to_unit(h) > density) continue;
            const f32 jx = hash_to_unit(hash32(h + 1)), jy = hash_to_unit(hash32(h + 2));
            const f32 radius = max_radius * (0.45f + 0.55f * hash_to_unit(hash32(h + 3)));
            // Work in the unwrapped frame of the current pixel's cell.
            const f32 centre_x = (static_cast<f32>(px / cell + ox) + 0.2f + 0.6f * jx) * static_cast<f32>(cell);
            const f32 centre_y = (static_cast<f32>(py / cell + oy) + 0.2f + 0.6f * jy) * static_cast<f32>(cell);
            const f32 dx = static_cast<f32>(px) + 0.5f - centre_x, dy = (static_cast<f32>(py) + 0.5f - centre_y) * 1.3f;
            const f32 d = std::sqrt(dx * dx + dy * dy);
            if (d > radius) continue;
            // Lit upper-left, shaded lower-right.
            const f32 facing = (-dx - dy) / (radius * 1.5f);
            result = 0.10f + 0.16f * facing - (d > radius - 0.9f ? 0.14f : 0.0f);
        }
    }
    return result;
}

Image make_ground(Terrain terrain, i32 size, u32 seed) {
    Image image(size, size);
    const f32 inv = 1.0f / static_cast<f32>(size);
    for (i32 y = 0; y < size; ++y) {
        for (i32 x = 0; x < size; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) * inv, v = (static_cast<f32>(y) + 0.5f) * inv;
            const f32 broad = periodic_fbm(u, v, 4, 4, seed + 1);
            const f32 mid = periodic_fbm(u, v, 16, 3, seed + 2);
            const f32 fine = periodic_noise(u * 128.0f, v * 128.0f, 128, seed + 3);
            Rgb c{1, 0, 1};
            switch (terrain) {
                case Terrain::Grass: {
                    // Short vertical streaks read as blades of grass.
                    const f32 blades = periodic_noise(u * 128.0f, v * 40.0f, 8, seed + 4) > 0.5f ? periodic_noise(u * 128.0f, v * 32.0f, 32, seed + 5) : 0.5f;
                    c = mix(rgb(84, 134, 54), rgb(118, 160, 64), broad);
                    c = scale(c, 0.88f + 0.14f * mid + 0.10f * (blades - 0.5f) + 0.06f * (fine - 0.5f));
                    break;
                }
                case Terrain::Forest: {
                    c = mix(rgb(44, 82, 40), rgb(68, 100, 46), broad);
                    c = mix(c, rgb(82, 78, 44), 0.35f * smoothstep(0.55f, 0.8f, mid));  // leaf litter
                    c = scale(c, 0.86f + 0.16f * mid + 0.12f * (fine - 0.5f));
                    break;
                }
                case Terrain::Dirt: {
                    c = mix(rgb(128, 102, 70), rgb(160, 132, 92), broad);
                    c = scale(c, 0.90f + 0.12f * mid + 0.08f * (fine - 0.5f));
                    c = scale(c, 1.0f + pebbles(x, y, size, 8, 0.30f, 2.6f, seed + 6));
                    break;
                }
                case Terrain::Sand: {
                    // Faint wind ripples.
                    const f32 ripple = std::sin((v * 20.0f + 1.6f * broad + 0.5f * u) * 2.0f * kPi);
                    c = mix(rgb(206, 186, 132), rgb(228, 212, 160), broad);
                    c = scale(c, 0.95f + 0.05f * mid + 0.025f * ripple + 0.05f * (fine - 0.5f));
                    c = scale(c, 1.0f + 0.6f * pebbles(x, y, size, 16, 0.12f, 1.8f, seed + 6));
                    break;
                }
                case Terrain::Mountain: {
                    // Neutral rock grain. The shader supplies colour and form
                    // from the relief; this only adds fine texture and strata.
                    const f32 strata = periodic_noise(u * 12.0f + 3.0f * broad, v * 96.0f, 96, seed + 7);
                    c = scale(rgb(132, 130, 126), 0.90f + 0.12f * mid + 0.10f * (fine - 0.5f) + 0.08f * (strata - 0.5f));
                    break;
                }
                case Terrain::Shallows: {
                    // River bed seen through clear water.
                    c = mix(rgb(132, 150, 128), rgb(168, 176, 140), broad);
                    c = scale(c, 0.94f + 0.08f * mid);
                    c = scale(c, 1.0f + 1.2f * pebbles(x, y, size, 8, 0.45f, 3.2f, seed + 6));
                    break;
                }
                case Terrain::Water: {
                    // Not a colour: two caustic patterns and a ripple field that
                    // the water shader scrolls against each other. Warping the
                    // lookup hides the lattice of the underlying noise.
                    const f32 wu = u + 0.06f * (periodic_fbm(u, v, 4, 3, seed + 10) - 0.5f);
                    const f32 wv = v + 0.06f * (periodic_fbm(u, v, 4, 3, seed + 11) - 0.5f);
                    const f32 a = periodic_ridged(wu, wv, 8, 2, seed + 8), b = periodic_ridged(wu, wv, 8, 2, seed + 9);
                    c = {a * a * a, b * b * b, mid};
                    break;
                }
                case Terrain::Count: break;
            }
            image.at(x, y) = to_color(c);
        }
    }
    return image;
}

Image make_noise_layer(i32 size, u32 seed) {
    Image image(size, size);
    const f32 inv = 1.0f / static_cast<f32>(size);
    for (i32 y = 0; y < size; ++y) {
        for (i32 x = 0; x < size; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) * inv, v = (static_cast<f32>(y) + 0.5f) * inv;
            image.at(x, y) = {to_byte(periodic_fbm(u, v, 8, 3, seed + 1)), to_byte(periodic_fbm(u, v, 8, 3, seed + 2)),
                              to_byte(periodic_fbm(u, v, 3, 3, seed + 3)), to_byte(periodic_fbm(u, v, 24, 2, seed + 4))};
        }
    }
    return image;
}

// --- sprite painting --------------------------------------------------------

// A small supersampled canvas. Shapes are described per sample by a function
// returning a colour; edges are antialiased by the final box downsample.
class Canvas {
public:
    static constexpr i32 kSamples = 4;

    Canvas(i32 width, i32 height) : width_(width), height_(height), data_(static_cast<size_t>(width * height * kSamples * kSamples), kClear) {}

    i32 width() const { return width_; }
    i32 height() const { return height_; }

    // Composites fn(x, y) over the canvas; x and y are in output pixels.
    template <class Fn>
    void paint(Fn&& fn) {
        const i32 sw = width_ * kSamples, sh = height_ * kSamples;
        for (i32 sy = 0; sy < sh; ++sy) {
            for (i32 sx = 0; sx < sw; ++sx) {
                const Rgba src = fn((static_cast<f32>(sx) + 0.5f) / kSamples, (static_cast<f32>(sy) + 0.5f) / kSamples);
                if (src.a <= 0.0f) continue;
                Rgba& dst = data_[static_cast<size_t>(sy * sw + sx)];  // premultiplied
                const f32 a = src.a > 1.0f ? 1.0f : src.a;
                dst.r = src.r * a + dst.r * (1.0f - a);
                dst.g = src.g * a + dst.g * (1.0f - a);
                dst.b = src.b * a + dst.b * (1.0f - a);
                dst.a = a + dst.a * (1.0f - a);
            }
        }
    }

    // Writes the downsampled, premultiplied result into `target` at (ox, oy).
    void blit(Image& target, i32 ox, i32 oy) const {
        const i32 sw = width_ * kSamples;
        const f32 norm = 1.0f / (kSamples * kSamples);
        for (i32 y = 0; y < height_; ++y) {
            for (i32 x = 0; x < width_; ++x) {
                Rgba sum = kClear;
                for (i32 sy = 0; sy < kSamples; ++sy) {
                    for (i32 sx = 0; sx < kSamples; ++sx) {
                        const Rgba& s = data_[static_cast<size_t>((y * kSamples + sy) * sw + x * kSamples + sx)];
                        sum.r += s.r;
                        sum.g += s.g;
                        sum.b += s.b;
                        sum.a += s.a;
                    }
                }
                target.at(ox + x, oy + y) = {to_byte(sum.r * norm), to_byte(sum.g * norm), to_byte(sum.b * norm), to_byte(sum.a * norm)};
            }
        }
    }

private:
    i32 width_, height_;
    std::vector<Rgba> data_;
};

void paint_shadow(Canvas& canvas, f32 cx, f32 cy, f32 rx, f32 ry, f32 strength) {
    canvas.paint([=](f32 x, f32 y) -> Rgba {
        const f32 dx = (x - cx) / rx, dy = (y - cy) / ry;
        const f32 d = dx * dx + dy * dy;
        if (d >= 1.0f) return kClear;
        return {0.02f, 0.05f, 0.04f, strength * (1.0f - smoothstep(0.35f, 1.0f, d))};
    });
}

f32 segment_distance(f32 x, f32 y, f32 ax, f32 ay, f32 bx, f32 by) {
    const f32 vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
    const f32 t = saturate((wx * vx + wy * vy) / (vx * vx + vy * vy + 1.0e-6f));
    const f32 dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}

struct Blob {
    f32 x, y, r, wobble, phase;
};

// A leafy mass built from overlapping spheres, cel-shaded in four tones.
void paint_canopy(Canvas& canvas, const std::vector<Blob>& blobs, const Rgb tones[4], Rgb outline, u32 seed) {
    canvas.paint([&](f32 x, f32 y) -> Rgba {
        f32 best = -1.0f, nx = 0, ny = 0, nz = 1, rim = 0;
        for (const Blob& b : blobs) {
            const f32 dx = x - b.x, dy = y - b.y;
            const f32 r = b.r * (1.0f + 0.07f * std::sin(std::atan2(dy, dx) * b.wobble + b.phase));
            const f32 d2 = dx * dx + dy * dy;
            if (d2 >= r * r) continue;
            const f32 z = std::sqrt(r * r - d2);
            // Lower blobs sit in front, like the near side of a crown.
            const f32 depth = z + b.y * 0.35f;
            if (depth > best) {
                best = depth;
                nx = dx / r;
                ny = dy / r;
                nz = z / r;
                rim = r - std::sqrt(d2);
            }
        }
        if (best < 0.0f) return kClear;
        if (rim < 1.1f) return opaque(outline);
        f32 light = nx * kLightX + ny * kLightY + nz * kLightZ;
        // Blotchy noise breaks the tone bands into leaf clusters.
        light += (value_noise(x * 0.42f, y * 0.42f, seed) - 0.5f) * 0.55f + (value_noise(x * 1.1f, y * 1.1f, seed + 1) - 0.5f) * 0.25f;
        const int tone = light < 0.05f ? 0 : (light < 0.42f ? 1 : (light < 0.78f ? 2 : 3));
        return opaque(tones[tone]);
    });
}

void paint_trunk(Canvas& canvas, f32 cx, f32 top, f32 bottom, f32 half_width) {
    canvas.paint([=](f32 x, f32 y) -> Rgba {
        // Flares slightly towards the ground.
        const f32 hw = half_width * (1.0f + 0.5f * smoothstep(bottom - 5.0f, bottom, y));
        if (y < top || y > bottom || std::fabs(x - cx) > hw) return kClear;
        return opaque(x < cx ? rgb(104, 76, 50) : rgb(72, 52, 36));
    });
}

Canvas make_broadleaf(Rng& rng, int palette) {
    static const Rgb kTones[3][4] = {
        {rgb(30, 76, 40), rgb(50, 110, 48), rgb(84, 144, 56), rgb(126, 176, 74)},
        {rgb(36, 82, 34), rgb(64, 118, 42), rgb(104, 150, 50), rgb(150, 182, 70)},
        {rgb(24, 70, 46), rgb(40, 100, 56), rgb(66, 132, 66), rgb(104, 164, 84)},
    };
    Canvas canvas(64, 76);
    const f32 cx = 32.0f, ground = 66.0f;
    paint_shadow(canvas, cx + 5.0f, ground + 1.0f, 21.0f, 8.0f, 0.42f);
    paint_trunk(canvas, cx, ground - 20.0f, ground, 2.6f);

    std::vector<Blob> blobs;
    const f32 crown_y = ground - 34.0f;
    blobs.push_back({cx, crown_y, rng.range(14.0f, 16.5f), 5.0f, rng.range(0.0f, 6.28f)});
    const int extra = rng.range_i(4, 6);
    for (int i = 0; i < extra; ++i) {
        const f32 angle = (static_cast<f32>(i) + rng.range(-0.3f, 0.3f)) * 2.0f * kPi / static_cast<f32>(extra);
        const f32 dist = rng.range(8.0f, 12.5f);
        blobs.push_back({cx + std::cos(angle) * dist * 1.05f, crown_y + std::sin(angle) * dist * 0.85f, rng.range(8.0f, 11.5f),
                         static_cast<f32>(rng.range_i(4, 7)), rng.range(0.0f, 6.28f)});
    }
    paint_canopy(canvas, blobs, kTones[palette % 3], rgb(18, 48, 30), rng.next_u32());
    return canvas;
}

Canvas make_bush(Rng& rng) {
    static const Rgb kTones[4] = {rgb(38, 84, 38), rgb(62, 118, 46), rgb(98, 150, 56), rgb(140, 180, 76)};
    Canvas canvas(34, 26);
    paint_shadow(canvas, 19.0f, 20.0f, 13.0f, 5.0f, 0.36f);
    std::vector<Blob> blobs;
    const int count = rng.range_i(3, 4);
    for (int i = 0; i < count; ++i) {
        blobs.push_back({rng.range(11.0f, 23.0f), rng.range(11.0f, 15.0f), rng.range(5.5f, 8.0f), static_cast<f32>(rng.range_i(4, 6)), rng.range(0.0f, 6.28f)});
    }
    paint_canopy(canvas, blobs, kTones, rgb(22, 52, 30), rng.next_u32());
    return canvas;
}

Canvas make_conifer(Rng& rng) {
    Canvas canvas(44, 80);
    const f32 cx = 22.0f, ground = 71.0f;
    paint_shadow(canvas, cx + 4.0f, ground + 1.0f, 15.0f, 6.0f, 0.42f);
    paint_trunk(canvas, cx, ground - 12.0f, ground, 2.0f);

    const Rgb dark = rgb(22, 60, 46), mid = rgb(36, 88, 58), light = rgb(62, 120, 72), edge = rgb(14, 40, 34);
    const int tiers = 4;
    const f32 lean = rng.range(-1.0f, 1.0f);
    const u32 seed = rng.next_u32();
    for (int i = 0; i < tiers; ++i) {
        const f32 bottom = ground - 7.0f - static_cast<f32>(i) * 13.5f;
        const f32 apex = bottom - 25.0f + static_cast<f32>(i) * 1.5f;
        const f32 half = 16.5f - static_cast<f32>(i) * 3.4f;
        const f32 axis = cx + lean * static_cast<f32>(i) * 0.5f;
        canvas.paint([=](f32 x, f32 y) -> Rgba {
            // Sawtooth hem makes the tier look like drooping boughs.
            const f32 saw = std::fabs(std::fmod(x * 0.21f + static_cast<f32>(i) * 0.37f, 1.0f) - 0.5f) * 5.0f;
            const f32 hem = bottom - saw;
            if (y < apex || y > hem) return kClear;
            const f32 width = half * (y - apex) / (bottom - apex);
            const f32 side = (x - axis) / (width + 1.0e-3f);
            if (std::fabs(side) > 1.0f) return kClear;
            if ((1.0f - std::fabs(side)) * width < 1.0f || hem - y < 0.9f) return opaque(edge);
            f32 light_amount = -side * 0.8f + (value_noise(x * 0.5f, y * 0.5f, seed + static_cast<u32>(i)) - 0.5f) * 0.9f;
            light_amount -= smoothstep(hem - 6.0f, hem, y) * 0.7f;  // underside in shade
            return opaque(light_amount > 0.25f ? light : (light_amount > -0.35f ? mid : dark));
        });
    }
    return canvas;
}

struct Boulder {
    f32 x, y, r, phase_a, phase_b, facet_phase;
};

// Low-poly boulders: a flat top and angular side facets, each lit as a plane.
void paint_boulders(Canvas& canvas, const std::vector<Boulder>& boulders, Rgb base, Rgb outline, Resource type, u32 seed) {
    for (const Boulder& b : boulders) {
        canvas.paint([&](f32 x, f32 y) -> Rgba {
            const f32 dx = x - b.x, dy = (y - b.y) * 1.25f;  // squashed: seen from above at an angle
            const f32 angle = std::atan2(dy, dx);
            const f32 r = b.r * (1.0f + 0.10f * std::sin(angle * 3.0f + b.phase_a) + 0.06f * std::sin(angle * 5.0f + b.phase_b));
            const f32 d = std::sqrt(dx * dx + dy * dy);
            if (d >= r) return kClear;
            if (d > r - 1.2f) return opaque(outline);

            f32 nx, ny, nz;
            if (d < r * 0.45f) {
                nx = -0.12f;
                ny = -0.18f;
                nz = 0.98f;
            } else {
                const f32 step = 2.0f * kPi / 6.0f;
                const f32 facet = (std::floor((angle + b.facet_phase) / step) + 0.5f) * step - b.facet_phase;
                nx = std::cos(facet) * 0.78f;
                ny = std::sin(facet) * 0.78f;
                nz = 0.62f;
            }
            const f32 light = nx * kLightX + ny * kLightY + nz * kLightZ;
            Rgb c = scale(base, 0.58f + 0.62f * light);

            // Ore shows as flecks in the host rock.
            const i32 cell_x = static_cast<i32>(std::floor(x / 2.5f)), cell_y = static_cast<i32>(std::floor(y / 2.5f));
            const f32 fleck = hash_to_unit(hash2(cell_x, cell_y, seed));
            if (type == Resource::Iron && fleck < 0.30f) c = mix(c, scale(rgb(206, 104, 44), 0.7f + 0.5f * light), 0.85f);
            if (type == Resource::Gold && fleck < 0.20f) c = fleck < 0.07f ? rgb(255, 240, 150) : scale(rgb(244, 196, 54), 0.75f + 0.45f * light);
            if (type == Resource::Stone && fleck < 0.12f) c = scale(c, 1.12f);
            return opaque(c);
        });
    }
}

Canvas make_deposit(Rng& rng, Resource type) {
    Canvas canvas(46, 40);
    paint_shadow(canvas, 25.0f, 29.0f, 20.0f, 8.5f, 0.40f);
    std::vector<Boulder> boulders;
    const int count = rng.range_i(3, 4);
    for (int i = 0; i < count; ++i) {
        boulders.push_back({rng.range(13.0f, 33.0f), rng.range(14.0f, 26.0f), rng.range(6.5f, 10.5f), rng.range(0.0f, 6.28f), rng.range(0.0f, 6.28f), rng.range(0.0f, 1.0f)});
    }
    // Paint far boulders first.
    std::sort(boulders.begin(), boulders.end(), [](const Boulder& a, const Boulder& b) { return a.y < b.y; });
    const Rgb base = type == Resource::Stone ? rgb(168, 168, 176) : (type == Resource::Iron ? rgb(120, 86, 74) : rgb(138, 132, 124));
    const Rgb outline = type == Resource::Iron ? rgb(52, 34, 30) : rgb(54, 54, 60);
    paint_boulders(canvas, boulders, base, outline, type, rng.next_u32());
    return canvas;
}

Canvas make_pebbles(Rng& rng) {
    Canvas canvas(22, 14);
    std::vector<Boulder> stones;
    const int count = rng.range_i(2, 3);
    for (int i = 0; i < count; ++i) {
        stones.push_back({rng.range(5.0f, 17.0f), rng.range(5.0f, 9.0f), rng.range(2.2f, 3.6f), rng.range(0.0f, 6.28f), rng.range(0.0f, 6.28f), rng.range(0.0f, 1.0f)});
    }
    std::sort(stones.begin(), stones.end(), [](const Boulder& a, const Boulder& b) { return a.y < b.y; });
    paint_shadow(canvas, 12.0f, 9.5f, 9.0f, 3.5f, 0.28f);
    paint_boulders(canvas, stones, rgb(150, 146, 140), rgb(70, 66, 62), Resource::None, rng.next_u32());
    return canvas;
}

// A fan of grass blades, optionally topped with flowers or seed heads.
Canvas make_tuft(Rng& rng, int kind) {
    const bool reed = kind == 2;
    Canvas canvas(20, reed ? 30 : 16);
    const f32 base_x = 10.0f, base_y = static_cast<f32>(canvas.height()) - 2.0f;
    const int blades = rng.range_i(5, 7);
    const Rgb dark = reed ? rgb(70, 104, 50) : rgb(62, 112, 44), light = reed ? rgb(128, 156, 78) : rgb(132, 178, 70);
    static const Rgb kPetals[3] = {rgb(246, 240, 222), rgb(244, 208, 72), rgb(196, 132, 214)};
    const Rgb petal = kPetals[rng.below(3)];
    for (int i = 0; i < blades; ++i) {
        const f32 spread = (static_cast<f32>(i) / static_cast<f32>(blades - 1) - 0.5f) * 2.0f;
        const f32 height = (reed ? rng.range(17.0f, 25.0f) : rng.range(6.0f, 11.0f)) * (1.0f - 0.25f * std::fabs(spread));
        const f32 tip_x = base_x + spread * (reed ? 5.0f : 7.5f) + rng.range(-1.0f, 1.0f), tip_y = base_y - height;
        const f32 root_x = base_x + spread * 2.0f;
        const Rgb colour = mix(dark, light, rng.next_f32());
        canvas.paint([=](f32 x, f32 y) -> Rgba {
            const f32 d = segment_distance(x, y, root_x, base_y, tip_x, tip_y);
            return d < 0.75f ? opaque(colour) : kClear;
        });
        const bool head = (kind == 1 && i % 2 == 0) || (reed && i % 3 == 0);
        if (head) {
            const Rgb head_colour = reed ? rgb(112, 76, 44) : petal;
            const f32 rx = reed ? 1.3f : 1.7f, ry = reed ? 3.2f : 1.7f;
            canvas.paint([=](f32 x, f32 y) -> Rgba {
                const f32 dx = (x - tip_x) / rx, dy = (y - tip_y) / ry;
                return dx * dx + dy * dy < 1.0f ? opaque(head_colour) : kClear;
            });
        }
    }
    return canvas;
}

// --- atlas packing ----------------------------------------------------------

class AtlasBuilder {
public:
    explicit AtlasBuilder(TerrainArt& art, i32 size) : art_(art) { art_.sprites = Image(size, size, Color{0, 0, 0, 0}); }

    // Adds a sprite whose ground contact point is (anchor_x, anchor_y) in pixels.
    void add(SpriteSet& set, const Canvas& canvas, f32 anchor_x, f32 anchor_y) {
        constexpr i32 kPad = 4;  // keeps neighbours from bleeding in at lower mip levels
        const i32 size = art_.sprites.width;
        if (x_ + canvas.width() + kPad > size) {
            x_ = kPad;
            y_ += row_height_ + kPad;
            row_height_ = 0;
        }
        ZL_CHECK(y_ + canvas.height() + kPad <= size, "sprite atlas is full");
        canvas.blit(art_.sprites, x_, y_);

        const auto norm = [size](i32 v) { return static_cast<u16>(static_cast<f32>(v) / static_cast<f32>(size) * 65535.0f + 0.5f); };
        const f32 tile = static_cast<f32>(kArtTilePixels);
        SpriteDef def;
        def.u0 = norm(x_);
        def.v0 = norm(y_);
        def.u1 = norm(x_ + canvas.width());
        def.v1 = norm(y_ + canvas.height());
        def.width = static_cast<f32>(canvas.width()) / tile;
        def.height = static_cast<f32>(canvas.height()) / tile;
        def.anchor_x = anchor_x / tile;
        def.anchor_y = anchor_y / tile;
        if (set.count == 0) set.first = static_cast<i32>(art_.defs.size());
        ++set.count;
        art_.defs.push_back(def);

        x_ += canvas.width() + kPad;
        row_height_ = std::max(row_height_, canvas.height());
    }

private:
    TerrainArt& art_;
    i32 x_ = 4, y_ = 4, row_height_ = 0;
};

}  // namespace

TerrainArt build_terrain_art(JobSystem& jobs, u32 seed) {
    TerrainArt art;

    // Every texture and sprite is independent, so they are drawn in parallel.
    const i32 ground_size = kArtTilePixels * kGroundRepeatTiles;
    art.ground.resize(static_cast<size_t>(kTerrainCount) + 1);
    jobs.parallel_for(kTerrainCount + 1, 1, [&](i32 begin, i32 end) {
        for (i32 t = begin; t < end; ++t) {
            art.ground[static_cast<size_t>(t)] = t == kNoiseLayer ? make_noise_layer(ground_size, seed + 4242u)
                                                                  : make_ground(static_cast<Terrain>(t), ground_size, seed + static_cast<u32>(t) * 977u);
        }
    });

    enum class Kind { Broadleaf, Conifer, Bush, Stone, Iron, Gold, Tuft, Flower, Pebble, Reed };
    struct Family {
        Kind kind;
        SpriteSet TerrainArt::* set;
        int variants;
        f32 anchor_x, anchor_y;  // ground contact point in sprite pixels
    };
    // A SpriteSet is a contiguous range, so each family's variants stay together.
    const Family families[] = {
        {Kind::Broadleaf, &TerrainArt::broadleaf, 6, 32.0f, 66.0f}, {Kind::Conifer, &TerrainArt::conifer, 4, 22.0f, 71.0f},
        {Kind::Bush, &TerrainArt::bush, 3, 17.0f, 20.0f},           {Kind::Stone, &TerrainArt::stone, 3, 23.0f, 27.0f},
        {Kind::Iron, &TerrainArt::iron, 3, 23.0f, 27.0f},           {Kind::Gold, &TerrainArt::gold, 3, 23.0f, 27.0f},
        {Kind::Tuft, &TerrainArt::tuft, 4, 10.0f, 14.0f},           {Kind::Flower, &TerrainArt::flower, 4, 10.0f, 14.0f},
        {Kind::Pebble, &TerrainArt::pebble, 3, 11.0f, 9.0f},        {Kind::Reed, &TerrainArt::reed, 3, 10.0f, 28.0f},
    };
    struct Item {
        const Family* family;
        int variant;
    };
    std::vector<Item> items;
    for (const Family& family : families) {
        for (int v = 0; v < family.variants; ++v) items.push_back({&family, v});
    }

    std::vector<std::unique_ptr<Canvas>> canvases(items.size());
    jobs.parallel_for(static_cast<i32>(items.size()), 1, [&](i32 begin, i32 end) {
        for (i32 i = begin; i < end; ++i) {
            // Each sprite has its own random stream, so results do not depend on scheduling.
            Rng rng(mix64(seed) + static_cast<u64>(i) * 0x9E3779B97F4A7C15ull);
            const Item& item = items[static_cast<size_t>(i)];
            Canvas canvas = [&]() -> Canvas {
                switch (item.family->kind) {
                    case Kind::Broadleaf: return make_broadleaf(rng, item.variant);
                    case Kind::Conifer: return make_conifer(rng);
                    case Kind::Bush: return make_bush(rng);
                    case Kind::Stone: return make_deposit(rng, Resource::Stone);
                    case Kind::Iron: return make_deposit(rng, Resource::Iron);
                    case Kind::Gold: return make_deposit(rng, Resource::Gold);
                    case Kind::Tuft: return make_tuft(rng, 0);
                    case Kind::Flower: return make_tuft(rng, 1);
                    case Kind::Pebble: return make_pebbles(rng);
                    case Kind::Reed: return make_tuft(rng, 2);
                }
                return Canvas(1, 1);
            }();
            canvases[static_cast<size_t>(i)] = std::make_unique<Canvas>(std::move(canvas));
        }
    });

    AtlasBuilder atlas(art, 1024);
    for (size_t i = 0; i < items.size(); ++i) {
        const Family& family = *items[i].family;
        atlas.add(art.*(family.set), *canvases[i], family.anchor_x, family.anchor_y);
    }
    return art;
}

}  // namespace zl
