#include "render/figure_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "gfx/gl.h"

namespace zl {

namespace {

// Each part of a figure is four texels in a row of a float texture:
//   0: a.xy, b.xy       screen position of the two ends, tiles from the ground point
//   1: ra, rb, depth of a, depth of b (towards the viewer)
//   2: height of a, height of b, roundness, material + 8 * flags
//   3: colour, group
constexpr i32 kTexelsPerPart = 4;
constexpr i32 kRowTexels = kMaxFigureParts * kTexelsPerPart;

// The shader names the materials by number.
static_assert(static_cast<int>(FigureMaterial::Cloth) == 0 && static_cast<int>(FigureMaterial::Skin) == 1 &&
              static_cast<int>(FigureMaterial::Leather) == 2 && static_cast<int>(FigureMaterial::Hair) == 3 &&
              static_cast<int>(FigureMaterial::Metal) == 4 && static_cast<int>(FigureMaterial::Count) <= 8,
              "update the material constants in the figure shader");
static_assert(FigurePartFlag::FlatCaps == 1 && FigurePartFlag::Unshadowed == 2, "update the flag tests in the figure shader");

// A point at height h throws its shadow this far across the ground, in tiles
// per tile of height. The direction is the terrain shader's light; the length
// is cut to about half so the shadow stays under the figure.
constexpr f32 kShadowReach = 0.55f;
constexpr Vec2 kShadowOnGround{0.50f / 0.60f * kShadowReach, 0.62f / 0.60f * kShadowReach};
// The same, as an offset on screen from the point that casts it.
constexpr Vec2 kShadowOnScreen{kShadowOnGround.x, kFigureUpScale + kShadowOnGround.y * kFigureDepthScale};

// Ground effects reach this far from the figure, in tiles.
constexpr f32 kRippleRadius = 0.80f;
constexpr f32 kMarkerRadius = 0.62f;
constexpr f32 kMarkerPixels = 13.0f;

struct FigureInstance {
    f32 x, y, w, h;                   // quad, in tiles
    f32 ox, oy, parts, waterline;     // ground point; number of parts; waterline in tiles
    f32 row, time, marker, unused;
};

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec4 aRect;
layout(location = 1) in vec4 aFigure;
layout(location = 2) in vec4 aExtra;
uniform vec4 uView;  // xy: clip scale, zw: clip offset
out vec2 vLocal;     // position relative to the figure's ground point, in tiles
flat out vec4 vInfo; // part count, waterline, time, marker
flat out int vRow;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 world = aRect.xy + aRect.zw * corner;
    vLocal = world - aFigure.xy;
    vInfo = vec4(aFigure.zw, aExtra.yz);
    vRow = int(aExtra.x + 0.5);
    gl_Position = vec4(world * uView.xy + uView.zw, 0.0, 1.0);
}
)";

const char* kFragment = R"(#version 330 core
uniform sampler2D uParts;
uniform float uPixel;     // tiles per screen pixel
uniform vec2 uShadow;     // screen offset of a shadow per tile of height
uniform vec2 uProjection; // depth and up scales, shared with the CPU projection
uniform vec3 uGround;     // ripple radius, marker radius, marker radius in pixels
in vec2 vLocal;
flat in vec4 vInfo;
flat in int vRow;
out vec4 oColor;

// The light and the palette of the painted art (docs/art-direction.md).
const vec3 LIGHT = vec3(-0.5013, -0.6216, 0.6016);
const vec3 SOOT = vec3(0.102, 0.086, 0.063);
const vec3 BONE = vec3(0.847, 0.812, 0.706);
const vec3 HAZE = vec3(0.56, 0.53, 0.47);
const int CLOTH = 0, SKIN = 1, LEATHER = 2, HAIR = 3, METAL = 4;

float hash(vec2 p) {
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Conservative screen-space bounds avoid the cone's square roots for parts
// that cannot touch this pixel. Keep the same guard for antialiasing/shadows.
bool nearPart(vec2 p, vec2 a, vec2 b, float ra, float rb, float guard) {
    vec2 halfSize = abs(b - a) * 0.5 + vec2(max(ra, rb) + guard);
    return all(lessThanEqual(abs(p - (a + b) * 0.5), halfSize));
}

// Height at the visible surface, not just at the cone's centreline. The
// inverse of the figure projection keeps the water cut level across a boot.
float surfaceHeight(vec4 height, float t, vec2 off, float bulge) {
    return mix(height.x, height.y, t) - off.y * uProjection.y + bulge * uProjection.x;
}

// Signed distance from p to a rounded cone. A rounded cone is the union of
// circles strung along a..b; `t` says which of them p is nearest to, `off` is
// the vector from its centre to p and `r` its radius.
float cone(vec2 p, vec2 a, vec2 b, float ra, float rb, bool flatCaps, out float t, out vec2 off, out float r) {
    vec2 ba = b - a, pa = p - a;
    float h = dot(ba, ba), dr = ra - rb;
    float along = h > 1.0e-10 ? dot(pa, ba) / h : 0.0;
    if (flatCaps) {
        t = clamp(along, 0.0, 1.0);
    } else if (h <= dr * dr + 1.0e-10) {
        t = dr > 0.0 ? 0.0 : 1.0;  // one end swallows the other
    } else {
        float across = abs(pa.x * ba.y - pa.y * ba.x) / h;
        t = clamp(along - across * dr / sqrt(h - dr * dr), 0.0, 1.0);
    }
    r = mix(ra, rb, t);
    off = p - (a + ba * t);
    float d = length(off) - r;
    if (flatCaps) d = max(d, max(-along, along - 1.0) * sqrt(h));
    return d;
}

// How much of the light another part keeps off the surface point (p, z):
// two steps along the ray towards the light, asking whether a part stands there.
float occlusion(vec2 p, float z, int count, float water) {
    vec2 p1 = p + LIGHT.xy * 0.030, p2 = p + LIGHT.xy * 0.075;
    float z1 = z + LIGHT.z * 0.030, z2 = z + LIGHT.z * 0.075;
    float dark = 0.0;
    for (int i = 0; i < count; ++i) {
        vec4 ends = texelFetch(uParts, ivec2(i * 4, vRow), 0);
        vec4 size = texelFetch(uParts, ivec2(i * 4 + 1, vRow), 0);
        bool near1 = nearPart(p1, ends.xy, ends.zw, size.x, size.y, 0.0);
        bool near2 = nearPart(p2, ends.xy, ends.zw, size.x, size.y, 0.0);
        if (!near1 && !near2) continue;
        vec4 more = texelFetch(uParts, ivec2(i * 4 + 2, vRow), 0);
        bool flatCaps = (int(more.w + 0.5) & 8) != 0;
        float t, r;
        vec2 off;
        if (near1 && cone(p1, ends.xy, ends.zw, size.x, size.y, flatCaps, t, off, r) < 0.0 && surfaceHeight(more, t, off, sqrt(max(r * r - dot(off, off), 0.0))) >= water) {
            float middle = mix(size.z, size.w, t), bulge = sqrt(max(r * r - dot(off, off), 0.0));
            dark = max(dark, smoothstep(0.004, 0.020, middle + bulge - z1) * step(middle - bulge, z1 + 0.12));
        }
        if (near2 && cone(p2, ends.xy, ends.zw, size.x, size.y, flatCaps, t, off, r) < 0.0 && surfaceHeight(more, t, off, sqrt(max(r * r - dot(off, off), 0.0))) >= water) {
            float middle = mix(size.z, size.w, t), bulge = sqrt(max(r * r - dot(off, off), 0.0));
            dark = max(dark, smoothstep(0.004, 0.020, middle + bulge - z2) * step(middle - bulge, z2 + 0.12));
        }
    }
    return dark;
}

// The colour of part i at p: its own colour in shadow, half tone, light and a
// sparing highlight, the steps broken up like brush strokes laid along the form.
// Also returns its distance, its group, and `rim`: 1 where its edge faces away
// from the light.
vec3 paint(int i, vec2 p, int count, float water, bool shadowed, out float d, out float group, out float rim) {
    vec4 ends = texelFetch(uParts, ivec2(i * 4, vRow), 0);
    vec4 size = texelFetch(uParts, ivec2(i * 4 + 1, vRow), 0);
    vec4 more = texelFetch(uParts, ivec2(i * 4 + 2, vRow), 0);
    vec4 tint = texelFetch(uParts, ivec2(i * 4 + 3, vRow), 0);
    int code = int(more.w + 0.5);
    int material = code & 7;
    float t, r;
    vec2 off;
    d = cone(p, ends.xy, ends.zw, size.x, size.y, (code & 8) != 0, t, off, r);
    group = tint.a;

    vec2 slope = off / max(r, 1.0e-5) * more.z;
    float steep = dot(slope, slope);
    if (steep > 0.97) slope *= sqrt(0.97 / steep);
    vec3 n = vec3(slope, sqrt(1.0 - dot(slope, slope)));
    float light = dot(n, LIGHT);

    // Brush marks: noise stretched along the part, in the part's own space so
    // that it moves with the limb.
    vec2 ba = ends.zw - ends.xy;
    float len = length(ba);
    vec2 axis = len > 1.0e-5 ? ba / len : vec2(0.0, 1.0);
    vec2 q = vec2(dot(p - ends.xy, axis), dot(p - ends.xy, vec2(-axis.y, axis.x)));
    float seed = float(i) * 7.13;
    float dab = noise(q * vec2(24.0, 58.0) + seed) - 0.5;
    float grain = material == CLOTH ? 0.30 : (material == LEATHER ? 0.20 : 0.10);
    if (material == HAIR) {
        dab = noise(q * vec2(8.0, 170.0) + seed) - 0.5;  // strands
        grain = 0.62;
    }
    light += dab * grain;
    if (shadowed && (code & 16) == 0) {
        float z = mix(size.z, size.w, t) + sqrt(max(r * r - dot(off, off), 0.0));
        light = mix(light, min(light, -0.25), occlusion(p, z, count, water));
    }

    vec3 base = tint.rgb;
    vec3 dark = mix(base * 0.50, SOOT, 0.34);
    vec3 half_tone = base * 0.77;
    vec3 high = mix(base, BONE, 0.40) * 1.08;
    float gloss = 0.22;
    if (material == SKIN) {
        dark = base * vec3(0.60, 0.45, 0.41);
        half_tone = base * vec3(0.86, 0.77, 0.73);
        gloss = 0.50;
    } else if (material == LEATHER) {
        dark = mix(base * 0.40, SOOT, 0.42);
        gloss = 0.55;
    } else if (material == HAIR) {
        high = base * 1.55 + 0.04;
        gloss = 0.65;
    } else if (material == METAL) {
        dark = mix(base * 0.34, SOOT, 0.3);
        high = BONE;
        gloss = 1.0;
    }
    const float W = 0.04;
    vec3 c = mix(dark, half_tone, smoothstep(0.02 - W, 0.02 + W, light));
    c = mix(c, base, smoothstep(0.40 - W, 0.40 + W, light));
    c = mix(c, high, gloss * smoothstep(0.84 - W, 0.84 + W, light));

    vec2 outward = off / max(length(off), 1.0e-6);
    rim = 1.0 - smoothstep(-0.35, 0.10, dot(outward, normalize(LIGHT.xy)));
    return c;
}

void main() {
    int count = int(vInfo.x + 0.5);
    float water = vInfo.y;
    vec2 p = vLocal;
    float aa = uPixel;

    // Pass over every part: which two are nearest the viewer here, how far the
    // pixel is inside the figure as a whole, and how much shadow the figure
    // throws on the ground at this point.
    int first = -1, second = -1;
    float zFirst = -1.0e9, zSecond = -1.0e9;
    float inside = 1.0e9, shadow = 0.0;
    for (int i = 0; i < count; ++i) {
        vec4 ends = texelFetch(uParts, ivec2(i * 4, vRow), 0);
        vec4 size = texelFetch(uParts, ivec2(i * 4 + 1, vRow), 0);
        vec4 more = texelFetch(uParts, ivec2(i * 4 + 2, vRow), 0);
        float t, r;
        vec2 off;
        if (nearPart(p, ends.xy, ends.zw, size.x, size.y, 0.5 * aa)) {
            float d = cone(p, ends.xy, ends.zw, size.x, size.y, (int(more.w + 0.5) & 8) != 0, t, off, r);
            float bulge = sqrt(max(r * r - dot(off, off), 0.0));
            if (water > 0.0) d = max(d, water - surfaceHeight(more, t, off, bulge));
            if (d < 0.5 * aa) {
                inside = min(inside, d);
                float z = mix(size.z, size.w, t) + bulge;
                if (z > zFirst) {
                    second = first;
                    zSecond = zFirst;
                    first = i;
                    zFirst = z;
                } else if (z > zSecond) {
                    second = i;
                    zSecond = z;
                }
            }
        }
        if (max(more.x, more.y) > water) {
            float ha = max(more.x - water, 0.0), hb = max(more.y - water, 0.0);
            vec2 a = ends.xy + ha * uShadow, b = ends.zw + hb * uShadow;
            if (!nearPart(p, a, b, size.x * 0.85, size.y * 0.85, 0.012 + 0.075 * max(ha, hb))) continue;
            float d2 = cone(p, a, b, size.x * 0.85, size.y * 0.85, false, t, off, r);
            // The further the shadow falls, the softer its edge.
            float soft = 0.012 + 0.075 * mix(ha, hb, t);
            shadow = max(shadow, 1.0 - smoothstep(-soft, soft, d2));
        }
    }

    // --- the ground under the figure: its shadow, ripples if it is wading,
    // and a ring to find it by when the camera is far away
    vec2 ground = vec2(p.x, p.y / uProjection.x);
    float reach = length(ground);
    vec3 under = SOOT;
    float underAlpha = shadow * 0.44;
    if (water > 0.0) {
        float ring = smoothstep(0.50, 0.95, sin(reach * 30.0 - vInfo.z * 4.5));
        float foam = ring * (1.0 - smoothstep(0.22, uGround.x, reach)) * smoothstep(0.0, 0.08, water) * 0.34;
        under = mix(under * underAlpha, BONE, foam) / max(mix(underAlpha, 1.0, foam), 1.0e-4);
        underAlpha = mix(underAlpha, 1.0, foam);
    }
    if (vInfo.w > 0.0) {
        float radius = max(uGround.y, uGround.z * aa);
        float width = max(0.035, 1.2 * aa);
        float ring = (1.0 - smoothstep(width, width + aa, abs(reach - radius))) * vInfo.w * 0.85;
        under = mix(under * underAlpha, BONE, ring) / max(mix(underAlpha, 1.0, ring), 1.0e-4);
        underAlpha = mix(underAlpha, 1.0, ring);
    }

    if (first < 0) {
        oColor = vec4(under * underAlpha, underAlpha);
        return;
    }

    // --- the figure itself
    float d1, g1, rim1;
    vec3 c1 = paint(first, p, count, water, true, d1, g1, rim1);
    float a1 = clamp(0.5 - d1 / aa, 0.0, 1.0);
    // A dark edge, not an outline: only where the nearest part ends on its
    // shadow side against something that is not the same surface.
    float edgeWidth = max(0.012, 0.8 * aa);
    float edge = smoothstep(-edgeWidth, -0.35 * edgeWidth, d1) * rim1 * 0.86;
    vec3 color = c1;
    if (second >= 0) {
        float d2, g2, rim2;
        vec3 c2 = paint(second, p, count, water, false, d2, g2, rim2);
        float gap = zFirst - zSecond;
        bool same = abs(g1 - g2) < 0.5;
        // Parts of one surface melt into each other; different surfaces meet
        // along a line one pixel wide.
        float band = same ? max(1.5 * aa, 0.014) : aa;
        if (same) edge *= smoothstep(0.02, 0.05, gap);
        color = mix(c2, mix(c1, SOOT, edge), a1 * (0.5 + 0.5 * smoothstep(0.0, band, gap)));
    } else {
        color = mix(c1, SOOT, edge);
    }

    // The same overcast, smoke-stained air as the terrain, laid on more thinly
    // so that the figure stands out from the ground.
    color = mix(vec3(dot(color, vec3(0.299, 0.587, 0.114))), color, 0.94);
    color = mix(color, HAZE, 0.035);

    float alpha = clamp(0.5 - inside / aa, 0.0, 1.0);
    oColor = vec4(color * alpha + under * underAlpha * (1.0 - alpha), alpha + underAlpha * (1.0 - alpha));
}
)";

}  // namespace

FigureRenderer::~FigureRenderer() {
    if (parts_) glDeleteTextures(1, &parts_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void FigureRenderer::init() {
    shader_.compile("figure", kVertex, kFragment);
    shader_.bind();
    glUniform1i(shader_.uniform("uParts"), 0);
    glUniform2f(shader_.uniform("uShadow"), kShadowOnScreen.x, kShadowOnScreen.y);
    glUniform2f(shader_.uniform("uProjection"), kFigureDepthScale, kFigureUpScale);
    glUniform3f(shader_.uniform("uGround"), kRippleRadius, kMarkerRadius, kMarkerPixels);
    u_view_ = shader_.uniform("uView");
    u_pixel_ = shader_.uniform("uPixel");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const GLsizei stride = sizeof(FigureInstance);
    for (GLuint attribute = 0; attribute < 3; ++attribute) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribPointer(attribute, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(static_cast<size_t>(attribute) * 4 * sizeof(f32)));
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);

    glGenTextures(1, &parts_);
    glBindTexture(GL_TEXTURE_2D, parts_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

void FigureRenderer::draw(const FigureDraw* figures, u32 count, const Camera2D& camera) {
    if (count == 0) return;
    const f32 pixel = 1.0f / camera.zoom;
    const Vec2 view_lo = camera.visible_min(), view_hi = camera.visible_max();

    std::vector<FigureInstance> instances;
    instances.reserve(count);
    texels_.clear();
    for (u32 k = 0; k < count; ++k) {
        const FigureDraw& item = figures[k];
        if (!item.figure || item.figure->parts.empty()) continue;
        const Figure& figure = *item.figure;
        const i32 parts = std::min(static_cast<i32>(figure.parts.size()), kMaxFigureParts);
        // A wading figure stands on the bed, so it sits lower on the screen
        // and the water cuts it off where the map's surface is.
        const f32 water = std::max(figure.waterline, 0.0f);
        const f32 sink = water * kFigureUpScale;

        const size_t row_start = texels_.size();
        texels_.resize(row_start + static_cast<size_t>(kRowTexels) * 4, 0.0f);
        f32* out = texels_.data() + row_start;
        Vec2 lo{-0.05f, -0.05f}, hi{0.05f, 0.05f};
        const auto grow = [&](Vec2 centre, f32 radius) {
            lo.x = std::min(lo.x, centre.x - radius);
            lo.y = std::min(lo.y, centre.y - radius);
            hi.x = std::max(hi.x, centre.x + radius);
            hi.y = std::max(hi.y, centre.y + radius);
        };
        for (i32 i = 0; i < parts; ++i) {
            const FigurePart& part = figure.parts[static_cast<size_t>(i)];
            Vec2 a = figure_to_screen(part.a), b = figure_to_screen(part.b);
            a.y += sink;
            b.y += sink;
            const f32 texel[16] = {
                a.x, a.y, b.x, b.y,
                part.ra, part.rb, figure_depth(part.a), figure_depth(part.b),
                part.a.z, part.b.z, part.roundness, static_cast<f32>(static_cast<int>(part.material) + 8 * part.flags),
                static_cast<f32>(part.color.r) / 255.0f, static_cast<f32>(part.color.g) / 255.0f, static_cast<f32>(part.color.b) / 255.0f, static_cast<f32>(part.group),
            };
            std::copy_n(texel, 16, out + static_cast<size_t>(i) * 16);
            grow(a, part.ra);
            grow(b, part.rb);
            // And the shadow it throws, with room for its soft edge.
            const f32 ha = std::max(part.a.z - water, 0.0f), hb = std::max(part.b.z - water, 0.0f);
            grow(a + kShadowOnScreen * ha, part.ra + 0.02f + 0.08f * ha);
            grow(b + kShadowOnScreen * hb, part.rb + 0.02f + 0.08f * hb);
        }
        if (water > 0.0f) {
            grow({0.0f, 0.0f}, kRippleRadius);
        }
        if (item.marker > 0.0f) {
            const f32 radius = std::max(kMarkerRadius, kMarkerPixels * pixel) + std::max(0.035f, 1.2f * pixel);
            grow({0.0f, 0.0f}, radius);
        }
        const f32 margin = 2.0f * pixel;
        lo -= Vec2{margin, margin};
        hi += Vec2{margin, margin};

        const Vec2 world_lo = item.position + lo, world_hi = item.position + hi;
        if (world_hi.x < view_lo.x || world_hi.y < view_lo.y || world_lo.x > view_hi.x || world_lo.y > view_hi.y) {
            texels_.resize(row_start);  // off screen
            continue;
        }
        FigureInstance instance;
        instance.x = world_lo.x;
        instance.y = world_lo.y;
        instance.w = hi.x - lo.x;
        instance.h = hi.y - lo.y;
        instance.ox = item.position.x;
        instance.oy = item.position.y;
        instance.parts = static_cast<f32>(parts);
        instance.waterline = water;
        instance.row = static_cast<f32>(instances.size());
        instance.time = item.time;
        instance.marker = item.marker;
        instance.unused = 0.0f;
        instances.push_back(instance);
    }
    if (instances.empty()) return;
    const i32 used_rows = static_cast<i32>(instances.size());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, parts_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (used_rows > rows_) {
        rows_ = std::max(used_rows, rows_ * 2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kRowTexels, rows_, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kRowTexels, used_rows, GL_RGBA, GL_FLOAT, texels_.data());

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const u32 wanted = static_cast<u32>(instances.size());
    if (wanted > instance_capacity_) {
        instance_capacity_ = wanted + wanted / 2;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(instance_capacity_) * static_cast<GLsizeiptr>(sizeof(FigureInstance)), nullptr, GL_STREAM_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(wanted) * static_cast<GLsizeiptr>(sizeof(FigureInstance)), instances.data());

    shader_.bind();
    const Vec2 scale = camera.clip_scale(), offset = camera.clip_offset();
    glUniform4f(u_view_, scale.x, scale.y, offset.x, offset.y);
    glUniform1f(u_pixel_, pixel);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(wanted));
    glBindVertexArray(0);
}

}  // namespace zl
