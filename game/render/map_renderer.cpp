#include "render/map_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string>

#include "core/rng.h"
#include "gfx/gl.h"
#include "math/noise.h"
#include "render/relief.h"

namespace zl {

namespace {

// Below this many pixels per tile, scenery sprites are smaller than a couple
// of pixels and the ground shader alone carries the picture.
constexpr f32 kDecorMinZoom = 2.5f;
constexpr f32 kChunkSize = 16.0f;
constexpr i32 kShadowResolution = 2048;

struct ShadowCaster {
    f32 x, y, z;
    f32 left, top, width, height;
    u16 u0, v0, u1, v1;
    f32 anchor;
};

// The terrain shader hard-codes the texture-array layer that holds noise.
static_assert(kNoiseLayer == 8, "update NOISE in the terrain shader");

const char* kTerrainVertex = R"(#version 330 core
layout(location = 0) in vec2 aChunk;
uniform sampler2D uHeights;
uniform vec2 uMapSize;
uniform vec4 uView;
uniform mat2 uRotation;
uniform float uDepthScale;
uniform float uUnit;
uniform float uStep;
uniform int uCells;
uniform int uWall;
uniform bool uShadowPass;
uniform vec4 uShadowBounds;
uniform vec2 uShadowLight;
uniform float uShadowHeight;
out vec2 vWorld;
out float vWall;
out float vHeight;
void main() {
    const int corners[6] = int[6](0, 1, 2, 2, 1, 3);
    int corner = corners[gl_VertexID % 6];
    vec2 p;
    float z;
    vWall = 0.0;
    if (uWall == 0) {
        int cell = gl_VertexID / 6;
        p = min(aChunk + (vec2(cell % uCells, cell / uCells) + vec2(corner & 1, corner >> 1)) * uStep, uMapSize);
        z = texture(uHeights, p / uMapSize).r;
    } else {
        // Continuous rock skirt closes the silhouette at the four map edges.
        int segment = gl_VertexID / 6;
        int xs = int(ceil(uMapSize.x / uStep)), ys = int(ceil(uMapSize.y / uStep));
        int edge = segment < xs ? 0 : (segment < xs + ys ? 1 : (segment < 2 * xs + ys ? 2 : 3));
        int start = edge == 0 ? 0 : (edge == 1 ? xs : (edge == 2 ? xs + ys : 2 * xs + ys));
        float t = min(float(segment - start + (corner & 1)) * uStep, edge % 2 == 0 ? uMapSize.x : uMapSize.y);
        p = edge == 0 ? vec2(t, 0.0) : (edge == 1 ? vec2(uMapSize.x, t) : (edge == 2 ? vec2(t, uMapSize.y) : vec2(0.0, t)));
        z = (corner >> 1) == 0 ? texture(uHeights, p / uMapSize).r : -1.5 * uUnit;
        vWall = 1.0;
    }
    vec2 r = uRotation * p;
    vec2 plane = vec2((r.x - r.y) * 0.7071067812, (r.x + r.y) * 0.3535533906 - z * 0.8660254038);
    float depth = -((r.x + r.y) * 0.6123724357 + z * 0.5) * uDepthScale;
    gl_Position = vec4(plane * uView.xy + uView.zw, depth, 1.0);
    if (uShadowPass) {
        vec2 lightPlane = (p - uShadowLight * z - uShadowBounds.xy) / uShadowBounds.zw;
        gl_Position = vec4(lightPlane * 2.0 - 1.0, 1.0 - 2.0 * z / uShadowHeight, 1.0);
    }
    vWorld = p;
    vHeight = z;
}
)";

// Materials remain in simulation coordinates on the displaced surface, so
// ground, water, overlays and the tile grid rotate together without swimming.
const char* kTerrainFragment = R"(
uniform sampler2D uTiles;         // r: terrain id, g: resource id, b: tile flags, a: height level (point sampled)
uniform sampler2D uRelief;        // rg: surface normal, b: height, a: cavity (ridge > 0.5 > valley)
uniform sampler2D uSurface;       // r: mountain altitude, g: cliff face, b: height level
uniform sampler2D uDepth;         // r: distance from water to shore, g: moisture
uniform sampler2DArray uGround;   // one tiling texture per terrain, then noise
uniform vec2 uMapSize;
uniform mat2 uRotation;
uniform float uTime;
uniform float uGrid;
uniform int uOverlay;             // 0: none, 1: height levels, 2: passability
uniform float uPixel;             // world units per screen pixel
uniform float uUnit;              // tiles per world unit
out vec4 oColor;
in vec2 vWorld;
in float vWall;
in float vHeight;

const float WATER = 0.0, SHALLOWS = 1.0, GRASS = 3.0, FOREST = 5.0, MOUNTAIN = 6.0, NOISE = 8.0;
const float GROUND_SCALE = 1.0 / 8.0;
// Colours the shader supplies itself, chosen to sit with the painted art
// (see "Colours the shader owns" in docs/art-direction.md).
const vec3 WATER_SHORE = vec3(0.37, 0.46, 0.41);
const vec3 WATER_MID = vec3(0.22, 0.32, 0.30);
const vec3 WATER_DEEP = vec3(0.10, 0.16, 0.17);
const vec3 WATER_GLINT = vec3(0.52, 0.50, 0.40);   // light patterns on the surface, tinted bone
const vec3 FOAM = vec3(0.78, 0.77, 0.67);          // scum along the shoreline
const vec3 SCRUB = vec3(0.72, 0.74, 0.46);         // sallow growth on mountain foothills
const vec3 HAZE = vec3(0.56, 0.53, 0.47);          // smoke in the air

float tileId(ivec2 c) { return floor(texelFetch(uTiles, c, 0).r * 255.0 + 0.5); }

vec3 groundColor(float id, vec2 uv, float moisture) {
    vec3 c = texture(uGround, vec3(uv, id)).rgb;
    // Vegetation is yellower on dry ground and deeper green where it is wet.
    vec3 climate = mix(vec3(1.12, 1.02, 0.78), vec3(0.88, 1.02, 0.96), moisture);
    if (id == GRASS) c *= climate;
    if (id == FOREST) c *= mix(vec3(1.0), climate, 0.6);
    return c;
}

void main() {
    vec3 LIGHT = normalize(vec3(uShadowLight, 1.0));
    vec2 p = vWorld;
    vec2 mapUV = p / uMapSize;
    vec2 uv = p * GROUND_SCALE;

    vec4 noise = texture(uGround, vec3(p / 6.0, NOISE));
    vec4 broad = texture(uGround, vec3(p / 41.0, NOISE));

    // Jitter the lookup position so tile borders wander instead of following
    // the grid.
    vec2 q = p + (noise.rg - 0.5) * 0.85 - 0.5;
    vec2 cell = floor(q);
    vec2 t = q - cell;
    ivec2 last = ivec2(uMapSize) - 1;
    ivec2 c0 = clamp(ivec2(cell), ivec2(0), last);
    ivec2 c1 = clamp(ivec2(cell) + 1, ivec2(0), last);
    vec4 id = vec4(tileId(c0), tileId(ivec2(c1.x, c0.y)), tileId(ivec2(c0.x, c1.y)), tileId(c1));
    vec4 w = vec4((1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y);

    // Bilinear coverage of each corner's terrain. Raising it to a power turns
    // the blend into a soft "largest coverage wins", which rounds corners and
    // straightens staircase diagonals.
    vec4 cover = vec4(dot(w, vec4(equal(id, vec4(id.x)))), dot(w, vec4(equal(id, vec4(id.y)))),
                      dot(w, vec4(equal(id, vec4(id.z)))), dot(w, vec4(equal(id, vec4(id.w)))));
    vec4 s = w * pow(cover, vec4(9.0));
    s /= s.x + s.y + s.z + s.w;

    vec4 relief = texture(uRelief, mapUV);
    vec4 surface = texture(uSurface, mapUV);
    vec4 hydro = texture(uDepth, mapUV);
    // Patches a dozen tiles across shift the grass between dry and lush tones,
    // so open ground is not one flat green.
    float moisture = clamp(hydro.g + (texture(uGround, vec3(p / 15.0, NOISE)).g - 0.5) * 0.8, 0.0, 1.0);

    float water = dot(w, vec4(equal(id, vec4(WATER))));
    float shallows = dot(w, vec4(equal(id, vec4(SHALLOWS))));
    float mountain = dot(w, vec4(equal(id, vec4(MOUNTAIN))));
    float rock = dot(s, vec4(equal(id, vec4(MOUNTAIN))));

    // --- water ---
    float depth = hydro.r * (255.0 / 16.0);  // tiles from shore
    vec2 drift = uTime * vec2(0.020, 0.013);
    float caustic = texture(uGround, vec3(p / 5.0 + drift, WATER)).r * texture(uGround, vec3(p / 3.3 - drift.yx + 0.31, WATER)).g;
    float ripple = texture(uGround, vec3(p / 2.3 + drift * 2.0, WATER)).b - 0.5;
    // Still, murky water: grey-green at the shore, near-black green in the deeps.
    vec3 waterColor = mix(mix(WATER_SHORE, WATER_MID, smoothstep(0.0, 1.6, depth)), WATER_DEEP, smoothstep(1.4, 7.0, depth));
    waterColor *= 1.0 + ripple * 0.12;
    waterColor += WATER_GLINT * caustic * 0.75 * (1.0 - 0.75 * smoothstep(1.0, 6.0, depth));
    vec3 bed = texture(uGround, vec3(uv, SHALLOWS)).rgb;
    vec3 shallowColor = mix(bed, WATER_SHORE, 0.38) * 1.10 + WATER_GLINT * caustic * 1.0;

    // --- land ---
    vec3 g0 = groundColor(id.x, uv, moisture);
    vec3 g1 = groundColor(id.y, uv, moisture);
    vec3 g2 = groundColor(id.z, uv, moisture);
    vec3 g3 = groundColor(id.w, uv, moisture);

    vec3 n = vec3(relief.rg * 2.0 - 1.0, 0.0);
    n.z = sqrt(max(0.0, 1.0 - dot(n.xy, n.xy)));
    float light = dot(n, LIGHT) / LIGHT.z;  // 1 on level ground
    // Ridges catch the light and valleys hold shade, beyond what the slope alone gives.
    float cavity = clamp((relief.a - 0.5) * 2.0, -1.0, 1.0);
    float cliff = surface.g;
    float shade = clamp(1.0 + (light - 1.0) * mix(0.42, 0.90, max(rock, cliff)), 0.69, 1.27);
    // On open ground the same term brightens the rim of a plateau and darkens the foot of its cliff.
    shade *= 1.0 + cavity * mix(0.20, 0.28, rock);
    shade *= 0.90 + 0.22 * broad.b;  // large-scale tonal variation hides texture repeats
    // Painterly light on rock: warm where lit, cool in shade.
    vec3 rockLight = mix(vec3(0.84, 0.90, 1.02), vec3(1.05, 1.00, 0.92), smoothstep(0.70, 1.20, light));

    // Rock is tinted by altitude: scrubby lower slopes, grey flanks, pale summits.
    float altitude = surface.r;
    vec3 rockTint = mix(mix(vec3(0.92, 0.88, 0.72), vec3(1.02, 1.00, 0.96), smoothstep(0.0, 0.30, altitude)),
                        vec3(1.20, 1.19, 1.20), smoothstep(0.30, 0.95, altitude)) * rockLight;
    float scrub = (1.0 - smoothstep(0.03, 0.20, altitude + (noise.b - 0.5) * 0.10)) * (0.35 + 0.5 * moisture);
    rockTint = mix(rockTint, SCRUB, scrub);

    // Damp, darker ground at the water's edge.
    float damp = smoothstep(0.08, 0.5, water + shallows);
    // Higher ground is paler and drier in tone, which sets plateaus apart at a glance.
    vec3 landShade = shade * (1.0 - 0.22 * damp) * mix(vec3(0.90, 0.95, 0.96), vec3(1.13, 1.09, 0.93), surface.b);

    vec3 color = vec3(0.0);
    color += s.x * (id.x == WATER ? waterColor : (id.x == SHALLOWS ? shallowColor : g0 * (id.x == MOUNTAIN ? rockTint : vec3(1.0)) * landShade));
    color += s.y * (id.y == WATER ? waterColor : (id.y == SHALLOWS ? shallowColor : g1 * (id.y == MOUNTAIN ? rockTint : vec3(1.0)) * landShade));
    color += s.z * (id.z == WATER ? waterColor : (id.z == SHALLOWS ? shallowColor : g2 * (id.z == MOUNTAIN ? rockTint : vec3(1.0)) * landShade));
    color += s.w * (id.w == WATER ? waterColor : (id.w == SHALLOWS ? shallowColor : g3 * (id.w == MOUNTAIN ? rockTint : vec3(1.0)) * landShade));

    // Cliff faces are bare rock, banded along the slope like exposed strata.
    float strata = 0.90 + 0.10 * sin(surface.b * 150.0 + noise.r * 5.0);
    vec3 face = texture(uGround, vec3(uv * 2.0, MOUNTAIN)).rgb * 2.0 * vec3(0.72, 0.67, 0.58) * rockLight * strata;
    color = mix(color, face * shade, cliff * (1.0 - smoothstep(0.2, 0.6, water)));

    // Scum breathing along the shoreline.
    float shore = smoothstep(0.30, 0.5, water) * (1.0 - smoothstep(0.5, 0.82, water)) * (1.0 - smoothstep(0.0, 0.3, shallows));
    float foam = shore * (0.55 + 0.45 * sin(uTime * 1.3 + noise.a * 12.0 + p.x * 0.7));
    color = mix(color, FOAM, foam * 0.35);

    // Direct sun is occluded; cool ambient sky light remains in the shadows.
    // Apply to water too, so banks and trees cast across the shoreline.
    float visibility = shadowVisibility(p, vHeight);
    if (uShadows) {
        // A face turned away from the sun has no direct illumination. Handle
        // grazing faces analytically rather than letting shadow-map precision
        // leave contour-shaped bands across a steep mountain flank.
        visibility *= smoothstep(-0.20, 0.20, dot(n, LIGHT));
    }
    color *= mix(vec3(0.52, 0.58, 0.66), vec3(1.0), visibility);

    // An overcast, smoke-stained light over everything: a little colour
    // drained from the ground and a thin warm-grey haze laid on top.
    color = mix(vec3(dot(color, vec3(0.299, 0.587, 0.114))), color, 0.86);
    color = mix(color, HAZE, 0.06);

    vec2 fromEdge = abs(fract(p) - 0.5);
    vec2 lineWidth = min(vec2(0.45), fwidth(p) * 1.1);
    float line = max(smoothstep(0.5 - lineWidth.x, 0.5, fromEdge.x), smoothstep(0.5 - lineWidth.y, 0.5, fromEdge.y));
    color *= 1.0 - 0.22 * line * uGrid;

    // Debug overlays show the gameplay data exactly as the simulation sees it:
    // per tile, with no blending.
    if (uOverlay > 0) {
        ivec2 tc = clamp(ivec2(floor(p)), ivec2(0), last);
        vec4 tile = texelFetch(uTiles, tc, 0);
        int kind = int(tile.r * 255.0 + 0.5), flags = int(tile.b * 255.0 + 0.5), level = int(tile.a * 255.0 + 0.5);
        bool walkable = (flags & 1) != 0, buildable = (flags & 2) != 0, ramp = (flags & 8) != 0;
        vec3 tint;
        float strength;
        if (uOverlay == 1) {
            tint = ramp ? vec3(1.0) : (level == 0 ? vec3(0.20, 0.45, 0.92) : (level == 1 ? vec3(0.98, 0.80, 0.20) : vec3(0.92, 0.30, 0.22)));
            strength = kind == 0 || kind == 6 ? 0.0 : (walkable ? 0.50 : 0.22);  // not on water or mountain
        } else {
            tint = ramp ? vec3(0.20, 0.90, 0.95) : (buildable ? vec3(0.25, 0.85, 0.30) : (walkable ? vec3(0.95, 0.85, 0.25) : vec3(0.90, 0.20, 0.20)));
            strength = 0.42;
        }
        color = mix(color, tint, strength);

        // A dark line on every tile border a unit cannot cross because of a cliff.
        if (walkable && !ramp) {
            vec2 f = fract(p);
            float width = max(0.07, 1.5 * uPixel);
            ivec2 side = clamp(tc + ivec2(f.x < 0.5 ? -1 : 1, 0), ivec2(0), last);
            ivec2 above = clamp(tc + ivec2(0, f.y < 0.5 ? -1 : 1), ivec2(0), last);
            vec4 a = texelFetch(uTiles, side, 0), b = texelFetch(uTiles, above, 0);
            int fa = int(a.b * 255.0 + 0.5), fb = int(b.b * 255.0 + 0.5);
            bool cliffA = (fa & 1) != 0 && (fa & 8) == 0 && int(a.a * 255.0 + 0.5) != level && min(f.x, 1.0 - f.x) < width;
            bool cliffB = (fb & 1) != 0 && (fb & 8) == 0 && int(b.a * 255.0 + 0.5) != level && min(f.y, 1.0 - f.y) < width;
            if (cliffA || cliffB) color = vec3(0.10, 0.02, 0.02);
        }
    }

    if (vWall > 0.5) {
        float strata = 0.78 + 0.09 * sin(vHeight * 12.0 / uUnit + noise.r * 3.0);
        color = texture(uGround, vec3(vec2((p.x + p.y) / 4.0, vHeight / (4.0 * uUnit)), MOUNTAIN)).rgb;
        color *= vec3(0.63, 0.56, 0.46) * strata;
    }
    oColor = vec4(color, 1.0);
}
)";

const char* kDepthFragment = R"(#version 330 core
void main() {}
)";

const char* kCasterVertex = R"(#version 330 core
layout(location = 0) in vec3 aGround;
layout(location = 1) in vec4 aRect;
layout(location = 2) in vec4 aUV;
layout(location = 3) in float aAnchor;
uniform vec2 uRight;
uniform vec4 uShadowBounds;
uniform vec2 uShadowLight;
uniform float uShadowHeight;
out vec2 vUV;
out float vY;
flat out float vAnchor;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 offset = aRect.xy + aRect.zw * corner;
    vec2 p = aGround.xy + uRight * offset.x;
    float z = aGround.z - offset.y / 0.866025404;
    vec2 lightPlane = (p - uShadowLight * z - uShadowBounds.xy) / uShadowBounds.zw;
    gl_Position = vec4(lightPlane * 2.0 - 1.0, 1.0 - 2.0 * z / uShadowHeight, 1.0);
    vUV = mix(aUV.xy, aUV.zw, corner);
    vY = corner.y;
    vAnchor = aAnchor;
}
)";

const char* kCasterFragment = R"(#version 330 core
uniform sampler2D uAtlas;
in vec2 vUV;
in float vY;
flat in float vAnchor;
void main() {
    // Painted ground shadows and transparent canvas never become casters.
    if (vY > vAnchor || texture(uAtlas, vUV).a < 0.45) discard;
}
)";

u32 tint(f32 brightness, f32 warmth) {
    const auto channel = [](f32 v) { return static_cast<u8>(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return pack_color(channel(brightness * (1.0f + warmth)), channel(brightness), channel(brightness * (1.0f - warmth)));
}

}  // namespace

MapRenderer::~MapRenderer() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (chunk_buffer_) glDeleteBuffers(1, &chunk_buffer_);
    if (shadow_fbo_) glDeleteFramebuffers(1, &shadow_fbo_);
    if (shadow_texture_) glDeleteTextures(1, &shadow_texture_);
    if (caster_vao_) glDeleteVertexArrays(1, &caster_vao_);
    if (caster_buffer_) glDeleteBuffers(1, &caster_buffer_);
}

void MapRenderer::init(JobSystem& jobs, const char* asset_dir) {
    reload_art(jobs, asset_dir);

    const std::string fragment = std::string("#version 330 core\n") + kShadowSamplingGLSL + kTerrainFragment;
    terrain_.compile("terrain", kTerrainVertex, fragment.c_str());
    terrain_depth_.compile("terrain shadow depth", kTerrainVertex, kDepthFragment);
    scenery_depth_.compile("scenery shadow depth", kCasterVertex, kCasterFragment);
    terrain_.bind();
    glUniform1i(terrain_.uniform("uTiles"), 0);
    glUniform1i(terrain_.uniform("uRelief"), 1);
    glUniform1i(terrain_.uniform("uDepth"), 2);
    glUniform1i(terrain_.uniform("uGround"), 3);
    glUniform1i(terrain_.uniform("uSurface"), 4);
    glUniform1i(terrain_.uniform("uHeights"), 5);
    u_view_ = terrain_.uniform("uView");
    u_rotation_ = terrain_.uniform("uRotation");
    u_depth_scale_ = terrain_.uniform("uDepthScale");
    u_step_ = terrain_.uniform("uStep");
    u_cells_ = terrain_.uniform("uCells");
    u_wall_ = terrain_.uniform("uWall");
    u_map_size_ = terrain_.uniform("uMapSize");
    u_time_ = terrain_.uniform("uTime");
    u_grid_ = terrain_.uniform("uGrid");
    u_overlay_ = terrain_.uniform("uOverlay");
    u_pixel_ = terrain_.uniform("uPixel");
    u_unit_ = terrain_.uniform("uUnit");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &chunk_buffer_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, chunk_buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vec2), nullptr);
    glVertexAttribDivisor(0, 1);
    glBindVertexArray(0);

    glGenFramebuffers(1, &shadow_fbo_);
    glGenTextures(1, &shadow_texture_);
    glBindTexture(GL_TEXTURE_2D, shadow_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kShadowResolution, kShadowResolution, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const f32 border[] = {1, 1, 1, 1};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_texture_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    ZL_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "shadow framebuffer is incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    shadow_view_.texture = shadow_texture_;

    glGenVertexArrays(1, &caster_vao_);
    glGenBuffers(1, &caster_buffer_);
    glBindVertexArray(caster_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, caster_buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ShadowCaster), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(ShadowCaster), reinterpret_cast<void*>(offsetof(ShadowCaster, left)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_SHORT, GL_TRUE, sizeof(ShadowCaster), reinterpret_cast<void*>(offsetof(ShadowCaster, u0)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(ShadowCaster), reinterpret_cast<void*>(offsetof(ShadowCaster, anchor)));
    for (u32 i = 0; i < 4; ++i) glVertexAttribDivisor(i, 1);
    glBindVertexArray(0);
    batch_.init();
}

void MapRenderer::reload_art(JobSystem& jobs, const char* asset_dir) {
    shadows_valid_ = false;
    art_ = load_terrain_art(jobs, asset_dir);
    ground_.create_array(art_.ground, TextureFilter::Trilinear, TextureWrap::Repeat);
    sprites_.create(art_.sprites, TextureFilter::Trilinear, TextureWrap::Clamp, kSpriteAtlasMipLevel);
    // CPU images are only needed during upload; keep the catalogue and flags.
    art_.ground.clear();
    art_.ground.shrink_to_fit();
    art_.sprites = {};
}

void MapRenderer::set_map(const TileMap& map, JobSystem& jobs) {
    shadows_valid_ = false;
    const i32 w = map.width, h = map.height;
    map_width_ = w;
    map_height_ = h;

    Image tiles(w, h), hydro(w, h);
    for (i32 i = 0; i < w * h; ++i) {
        const size_t k = static_cast<size_t>(i);
        tiles.pixels[k] = {static_cast<u8>(map.terrain[k]), static_cast<u8>(map.resource[k]), map.flags[k], map.level[k]};
        hydro.pixels[k] = {map.shore_dist[k], static_cast<u8>(saturate(map.moisture[k]) * 255.0f + 0.5f), 0, 255};
    }
    tiles_.create(tiles, TextureFilter::Nearest, TextureWrap::Clamp);
    depth_.create(hydro, TextureFilter::Linear, TextureWrap::Clamp);

    Relief relief = build_relief(map, jobs);
    relief_.create(relief.shape, TextureFilter::Linear, TextureWrap::Clamp);
    surface_.create(relief.surface, TextureFilter::Linear, TextureWrap::Clamp);
    height_range_ = relief.height_range;
    tiles_per_unit_ = static_cast<f32>(map.resolution);
    height_width_ = relief.shape.width;
    height_height_ = relief.shape.height;
    heightfield_ = std::move(relief.heights);
    for (f32& height : heightfield_) height *= tiles_per_unit_;
    heights_.create_float(height_width_, height_height_, heightfield_.data());

    chunks_.clear();
    const f32 scale = static_cast<f32>(height_width_) / static_cast<f32>(w);
    for (i32 y = 0; y < h; y += static_cast<i32>(kChunkSize)) {
        for (i32 x = 0; x < w; x += static_cast<i32>(kChunkSize)) {
            Chunk chunk{{static_cast<f32>(x), static_cast<f32>(y)}, 1.0e9f, 0.0f};
            // Include the filter's neighbouring texels for a conservative bound.
            const i32 x0 = std::max(0, static_cast<i32>(static_cast<f32>(x) * scale) - 1);
            const i32 y0 = std::max(0, static_cast<i32>(static_cast<f32>(y) * scale) - 1);
            const i32 x1 = std::min(height_width_ - 1, static_cast<i32>((static_cast<f32>(x) + kChunkSize) * scale) + 1);
            const i32 y1 = std::min(height_height_ - 1, static_cast<i32>((static_cast<f32>(y) + kChunkSize) * scale) + 1);
            for (i32 hy = y0; hy <= y1; ++hy) {
                for (i32 hx = x0; hx <= x1; ++hx) {
                    const f32 z = heightfield_[static_cast<size_t>(hy * height_width_ + hx)];
                    chunk.low = std::min(chunk.low, z);
                    chunk.high = std::max(chunk.high, z);
                }
            }
            chunks_.push_back(chunk);
        }
    }

    build_decor(map);
}

void MapRenderer::build_decor(const TileMap& map) {
    decor_.clear();
    decor_.reserve(static_cast<size_t>(map.tile_count() / 3));
    const u32 seed = static_cast<u32>(mix64(map.seed));

    const auto add = [&](const SpriteSet& set, u32 pick, f32 gx, f32 gy, f32 scale, u32 color) {
        const SpriteDef& d = art_.defs[static_cast<size_t>(set.first + static_cast<i32>(pick % static_cast<u32>(set.count)))];
        SpriteInstance s;
        // Offsets stay screen-aligned; the anchor itself lives in the world.
        s.x = -d.anchor_x * scale;
        s.y = -d.anchor_y * scale;
        s.w = d.width * scale;
        s.h = d.height * scale;
        s.u0 = d.u0;
        s.v0 = d.v0;
        s.u1 = d.u1;
        s.v1 = d.v1;
        s.color = color;
        s.anchor = d.anchor_y / d.height;
        const bool casts = set.first == art_.broadleaf.first || set.first == art_.conifer.first ||
            set.first == art_.bush.first || set.first == art_.stone.first || set.first == art_.iron.first ||
            set.first == art_.gold.first || set.first == art_.boulder.first;
        decor_.push_back({{gx, gy}, ground_height({gx, gy}), s, casts});
    };

    const f32 band = std::max(map.mountain_level - map.sea_level, 1.0e-4f);
    f32 highest = map.mountain_level + 1.0e-4f;
    for (const f32 e : map.elevation) highest = std::max(highest, e);

    for (i32 y = 0; y < map.height; ++y) {
        for (i32 x = 0; x < map.width; ++x) {
            const size_t i = static_cast<size_t>(map.index(x, y));
            // A handful of independent random numbers for this tile.
            const u32 h0 = hash2(x, y, seed);
            const u32 h1 = hash32(h0 ^ 0xA511E9B3u), h2 = hash32(h1 + 0x68E31DA4u), h3 = hash32(h2 + 0xB5297A4Du);
            const f32 r0 = hash_to_unit(h0), r1 = hash_to_unit(h1), r2 = hash_to_unit(h2), r3 = hash_to_unit(h3);
            const f32 cx = static_cast<f32>(x) + 0.5f, cy = static_cast<f32>(y) + 0.5f;

            if (map.resource[i] != Resource::None) {
                const SpriteSet& set = map.resource[i] == Resource::Stone ? art_.stone : (map.resource[i] == Resource::Iron ? art_.iron : art_.gold);
                add(set, h3, cx + (r0 - 0.5f) * 0.2f, cy + 0.2f + (r1 - 0.5f) * 0.2f, 0.95f + 0.25f * r2, tint(0.94f + 0.06f * r3, 0.0f));
                continue;
            }

            switch (map.terrain[i]) {
                case Terrain::Forest: {
                    // Conifers take over on higher, drier ground.
                    const f32 altitude = (map.elevation[i] - map.sea_level) / band;
                    const f32 conifer = smoothstep(0.45f, 0.80f, altitude + (0.5f - map.moisture[i]) * 0.5f);
                    const int trees = r3 < 0.40f ? 2 : 1;
                    for (int k = 0; k < trees; ++k) {
                        const u32 hk = hash32(h3 + static_cast<u32>(k) * 0x9E3779B9u);
                        const f32 a = hash_to_unit(hk), b = hash_to_unit(hash32(hk + 1)), c = hash_to_unit(hash32(hk + 2)), d = hash_to_unit(hash32(hk + 3));
                        const bool is_conifer = hash_to_unit(hash32(hk + 4)) < conifer;
                        add(is_conifer ? art_.conifer : art_.broadleaf, hash32(hk + 5), cx + (a - 0.5f) * 0.9f, cy + (b - 0.5f) * 0.9f,
                            0.80f + 0.35f * c, tint(0.84f + 0.16f * d, (hash_to_unit(hash32(hk + 6)) - 0.5f) * 0.10f));
                    }
                    break;
                }
                case Terrain::Rocks: {
                    add(art_.boulder, h3, cx + (r0 - 0.5f) * 0.5f, cy + 0.25f + (r1 - 0.5f) * 0.4f, 0.85f + 0.45f * r2, tint(0.88f + 0.12f * r3, 0.02f));
                    break;
                }
                case Terrain::Grass: {
                    bool waterside = false;
                    for (int k = 0; k < 4 && !waterside; ++k) {
                        const i32 nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);
                        waterside = map.in_bounds(nx, ny) && map.terrain[static_cast<size_t>(map.index(nx, ny))] == Terrain::Water;
                    }
                    // Ground cover grows in drifts rather than evenly: flower
                    // meadows, thickets of shrubs and swathes of long grass.
                    const f32 flowers = 0.015f + 0.34f * smoothstep(0.35f, 0.80f, simplex2(cx / 9.0f, cy / 9.0f, seed + 1u));
                    const f32 shrubs = 0.006f + 0.16f * smoothstep(0.45f, 0.85f, simplex2(cx / 11.0f, cy / 11.0f, seed + 2u));
                    const f32 grass = 0.07f + 0.34f * smoothstep(0.10f, 0.70f, simplex2(cx / 6.0f, cy / 6.0f, seed + 3u));
                    const f32 gx = cx + (r1 - 0.5f) * 0.8f, gy = cy + (r2 - 0.5f) * 0.8f;
                    if (waterside && r0 < 0.45f) add(art_.reed, h3, gx, gy, 0.9f + 0.3f * r3, tint(0.95f, 0.0f));
                    else if (r0 < shrubs) add(art_.bush, h3, gx, gy, 0.9f + 0.4f * r3, tint(0.9f + 0.1f * r3, 0.0f));
                    else if (r0 < shrubs + flowers) add(art_.flower, h3, gx, gy, 0.9f + 0.3f * r3, tint(1.0f, 0.0f));
                    else if (r0 < shrubs + flowers + grass) add(art_.tuft, h3, gx, gy, 0.8f + 0.4f * r3, tint(0.92f + 0.08f * r3, 0.0f));
                    break;
                }
                case Terrain::Dirt: {
                    const f32 stones = 0.04f + 0.22f * smoothstep(0.30f, 0.80f, simplex2(cx / 7.0f, cy / 7.0f, seed + 4u));
                    if (r0 < stones) add(art_.pebble, h3, cx + (r1 - 0.5f) * 0.8f, cy + (r2 - 0.5f) * 0.8f, 0.8f + 0.5f * r3, tint(0.95f, 0.04f));
                    else if (r0 < stones + 0.05f) add(art_.tuft, h3, cx + (r1 - 0.5f) * 0.8f, cy + (r2 - 0.5f) * 0.8f, 0.8f + 0.3f * r3, tint(0.9f, 0.08f));
                    break;
                }
                case Terrain::Sand:
                    if (r0 < 0.03f) add(art_.pebble, h3, cx + (r1 - 0.5f) * 0.8f, cy + (r2 - 0.5f) * 0.8f, 0.7f + 0.4f * r3, tint(1.0f, 0.03f));
                    break;
                case Terrain::Shallows:
                    if (r0 < 0.16f) add(art_.pebble, h3, cx + (r1 - 0.5f) * 0.8f, cy + (r2 - 0.5f) * 0.8f, 0.9f + 0.5f * r3, tint(0.9f, -0.04f));
                    break;
                default: break;
            }
        }
    }

    for (i32 view = 0; view < 4; ++view) {
        auto& order = decor_order_[view];
        order.resize(decor_.size());
        std::iota(order.begin(), order.end(), 0u);
        IsometricCamera camera;
        camera.rotation = view;
        std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
            const Vec2 pa = camera.rotate(decor_[a].ground), pb = camera.rotate(decor_[b].ground);
            return pa.x + pa.y < pb.x + pb.y;
        });
    }
}

f32 MapRenderer::ground_height(Vec2 position) const {
    if (heightfield_.empty()) return 0.0f;
    // Match GL_LINEAR sampling exactly, including the half-texel offset.
    const f32 x = clamp(position.x * static_cast<f32>(height_width_) / static_cast<f32>(map_width_) - 0.5f, 0.0f, static_cast<f32>(height_width_ - 1));
    const f32 y = clamp(position.y * static_cast<f32>(height_height_) / static_cast<f32>(map_height_) - 0.5f, 0.0f, static_cast<f32>(height_height_ - 1));
    const i32 ix = static_cast<i32>(x), iy = static_cast<i32>(y);
    const i32 nx = std::min(ix + 1, height_width_ - 1), ny = std::min(iy + 1, height_height_ - 1);
    const auto at = [&](i32 px, i32 py) { return heightfield_[static_cast<size_t>(py * height_width_ + px)]; };
    return lerp(lerp(at(ix, iy), at(nx, iy), x - static_cast<f32>(ix)),
                lerp(at(ix, ny), at(nx, ny), x - static_cast<f32>(ix)), y - static_cast<f32>(iy));
}

f32 MapRenderer::mesh_step(f32 zoom) const {
    f32 step = tiles_per_unit_ / static_cast<f32>(kReliefTexelsPerUnit);
    while (step * zoom < 6.0f && step < 4.0f) step *= 2.0f;
    return step;
}

f32 MapRenderer::surface_height(Vec2 position, f32 zoom) const {
    const f32 step = mesh_step(zoom);
    const f32 x = clamp(position.x, 0.0f, static_cast<f32>(map_width_) - 1.0e-4f);
    const f32 y = clamp(position.y, 0.0f, static_cast<f32>(map_height_) - 1.0e-4f);
    const f32 x0 = std::floor(x / step) * step, y0 = std::floor(y / step) * step;
    const f32 x1 = std::min(x0 + step, static_cast<f32>(map_width_)), y1 = std::min(y0 + step, static_cast<f32>(map_height_));
    const f32 fx = (x - x0) / (x1 - x0), fy = (y - y0) / (y1 - y0);
    const f32 h10 = ground_height({x1, y0}), h01 = ground_height({x0, y1});
    if (fx + fy <= 1.0f) {
        const f32 h00 = ground_height({x0, y0});
        return h00 + (h10 - h00) * fx + (h01 - h00) * fy;
    }
    const f32 h11 = ground_height({x1, y1});
    return h11 + (h01 - h11) * (1.0f - fx) + (h10 - h11) * (1.0f - fy);
}

bool MapRenderer::pick(const IsometricCamera& camera, Vec2 screen, Vec2& world) const {
    const f32 top = height_range_ * tiles_per_unit_ + 1.0f;
    const Vec2 origin = camera.screen_to_world(screen, top);
    const Vec2 direction = camera.screen_to_world(screen, top - 1.0f) - origin;
    f32 enter = 0.0f, leave = top + 0.01f;
    const auto clip = [&](f32 start, f32 delta, f32 size) {
        if (std::fabs(delta) < 1.0e-6f) return start >= 0 && start <= size;
        f32 a = -start / delta, b = (size - start) / delta;
        if (a > b) std::swap(a, b);
        enter = std::max(enter, a);
        leave = std::min(leave, b);
        return enter <= leave;
    };
    if (!clip(origin.x, direction.x, static_cast<f32>(map_width_)) ||
        !clip(origin.y, direction.y, static_cast<f32>(map_height_))) return false;

    // Visit mesh cells from the eye towards the ground and intersect their
    // actual triangles. Unlike fixed-step marching this also catches a ray
    // grazing a narrow ridge, and returns the foremost of overlapping slopes.
    const f32 step = mesh_step(camera.zoom);
    const i32 columns = static_cast<i32>(std::ceil(static_cast<f32>(map_width_) / step));
    const i32 rows = static_cast<i32>(std::ceil(static_cast<f32>(map_height_) / step));
    const Vec2 first = origin + direction * (enter + 1.0e-4f);
    i32 cx = clamp(static_cast<i32>(std::floor(first.x / step)), 0, columns - 1);
    i32 cy = clamp(static_cast<i32>(std::floor(first.y / step)), 0, rows - 1);
    for (i32 visited = 0; visited < columns + rows + 2; ++visited) {
        if (cx < 0 || cy < 0 || cx >= columns || cy >= rows || enter > leave + 1.0e-4f) break;
        const f32 x0 = static_cast<f32>(cx) * step, y0 = static_cast<f32>(cy) * step;
        const f32 x1 = std::min(x0 + step, static_cast<f32>(map_width_)), y1 = std::min(y0 + step, static_cast<f32>(map_height_));
        const f32 tx = ((direction.x > 0 ? x1 : x0) - origin.x) / direction.x;
        const f32 ty = ((direction.y > 0 ? y1 : y0) - origin.y) / direction.y;
        const f32 end = std::min(leave, std::min(tx, ty));
        const f32 h00 = ground_height({x0, y0}), h10 = ground_height({x1, y0});
        const f32 h01 = ground_height({x0, y1}), h11 = ground_height({x1, y1});
        f32 nearest = leave + 1.0f;
        for (i32 triangle = 0; triangle < 2; ++triangle) {
            const f32 ax = (triangle == 0 ? h10 - h00 : h11 - h01) / (x1 - x0);
            const f32 ay = (triangle == 0 ? h01 - h00 : h11 - h10) / (y1 - y0);
            const f32 base = triangle == 0 ? h00 : h11 - ax * (x1 - x0) - ay * (y1 - y0);
            const f32 denominator = 1.0f + ax * direction.x + ay * direction.y;
            if (std::fabs(denominator) < 1.0e-7f) continue;
            const f32 t = (top - base - ax * (origin.x - x0) - ay * (origin.y - y0)) / denominator;
            if (t < enter - 1.0e-4f || t > end + 1.0e-4f) continue;
            const Vec2 p = origin + direction * t;
            const f32 diagonal = (p.x - x0) / (x1 - x0) + (p.y - y0) / (y1 - y0);
            if ((triangle == 0 && diagonal <= 1.0001f) || (triangle == 1 && diagonal >= 0.9999f)) nearest = std::min(nearest, t);
        }
        if (nearest <= leave) {
            world = origin + direction * nearest;
            return true;
        }
        if (tx <= ty + 1.0e-5f) cx += direction.x > 0 ? 1 : -1;
        if (ty <= tx + 1.0e-5f) cy += direction.y > 0 ? 1 : -1;
        enter = end;
    }
    return false;
}

void MapRenderer::frame_map(IsometricCamera& camera) const {
    Vec2 lo{1.0e9f, 1.0e9f}, hi{-1.0e9f, -1.0e9f};
    // Project each chunk's height bounds, not just the four ground corners:
    // summits can extend beyond the map's diamond, especially on small maps.
    for (const Chunk& chunk : chunks_) {
        for (i32 corner = 0; corner < 4; ++corner) {
            const Vec2 p{std::min(chunk.origin.x + static_cast<f32>(corner & 1) * kChunkSize, static_cast<f32>(map_width_)),
                         std::min(chunk.origin.y + static_cast<f32>(corner >> 1) * kChunkSize, static_cast<f32>(map_height_))};
            for (f32 z : {-1.5f * tiles_per_unit_, chunk.high + 3.0f}) {
                const Vec2 q = camera.project(p, z);
                lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y);
                hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y);
            }
        }
    }
    camera.center_height = ground_height({static_cast<f32>(map_width_) * 0.5f, static_cast<f32>(map_height_) * 0.5f});
    camera.center = camera.unproject((lo + hi) * 0.5f, camera.center_height);
    camera.zoom = std::min(camera.viewport.x / (hi.x - lo.x), camera.viewport.y / (hi.y - lo.y)) * 0.94f;
}

void MapRenderer::update_shadows(const IsometricCamera& camera) {
    const f32 top = height_range_ * tiles_per_unit_ + 8.0f;
    Vec2 low{1.0e9f, 1.0e9f}, high{-1.0e9f, -1.0e9f};
    for (i32 corner = 0; corner < 4; ++corner) {
        const Vec2 screen{(corner & 1) ? camera.viewport.x : 0.0f, (corner & 2) ? camera.viewport.y : 0.0f};
        for (f32 height : {0.0f, top}) {
            const Vec2 p = camera.screen_to_world(screen, height);
            low.x = std::min(low.x, p.x); low.y = std::min(low.y, p.y);
            high.x = std::max(high.x, p.x); high.y = std::max(high.y, p.y);
        }
    }
    low.x = clamp(low.x, 0.0f, static_cast<f32>(map_width_));
    low.y = clamp(low.y, 0.0f, static_cast<f32>(map_height_));
    high.x = clamp(high.x, 0.0f, static_cast<f32>(map_width_));
    high.y = clamp(high.y, 0.0f, static_cast<f32>(map_height_));
    // The map is projected along sun rays onto z=0. Fit visible receivers at
    // every elevation; select casters in this light-space volume, including
    // offscreen mountains and trees whose shadows enter the camera view.
    low -= Vec2{4, 4};
    high -= shadow_view_.light * top;
    high += Vec2{4, 4};
    const f32 span = std::ceil(std::max(high.x - low.x, high.y - low.y) / 8.0f) * 8.0f;
    const f32 texel = span / static_cast<f32>(kShadowResolution);
    const Vec2 origin{std::floor(low.x / texel) * texel, std::floor(low.y / texel) * texel};
    const f32 step = mesh_step(camera.zoom);
    if (shadows_valid_ && shadow_rotation_ == camera.rotation && shadow_step_ == step &&
        shadow_view_.origin.x == origin.x && shadow_view_.origin.y == origin.y && shadow_view_.size.x == span) return;
    shadow_view_.origin = origin;
    shadow_view_.size = {span, span};
    shadow_view_.height = top;
    shadow_rotation_ = camera.rotation;
    shadow_step_ = step;

    GLint draw_fbo = 0, read_fbo = 0, viewport[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
    glViewport(0, 0, kShadowResolution, kShadowResolution);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    const auto intersects = [&](Vec2 lo, Vec2 hi) {
        return hi.x >= origin.x && hi.y >= origin.y && lo.x <= origin.x + span && lo.y <= origin.y + span;
    };
    std::vector<Vec2> caster_chunks;
    for (const Chunk& chunk : chunks_) {
        Vec2 lo{1.0e9f, 1.0e9f}, hi{-1.0e9f, -1.0e9f};
        for (i32 corner = 0; corner < 4; ++corner) {
            const Vec2 p = chunk.origin + Vec2{(corner & 1) ? kChunkSize : 0.0f, (corner & 2) ? kChunkSize : 0.0f};
            for (f32 z : {chunk.low, chunk.high}) {
                const Vec2 q = p - shadow_view_.light * z;
                lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y);
                hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y);
            }
        }
        if (intersects(lo, hi)) caster_chunks.push_back(chunk.origin);
    }
    terrain_depth_.bind();
    bind_shadows(terrain_depth_, &shadow_view_);
    glUniform1i(terrain_depth_.uniform("uShadowPass"), 1);
    glUniform1i(terrain_depth_.uniform("uHeights"), 5);
    glUniform2f(terrain_depth_.uniform("uMapSize"), static_cast<f32>(map_width_), static_cast<f32>(map_height_));
    glUniform1f(terrain_depth_.uniform("uStep"), step);
    const i32 cells = static_cast<i32>(kChunkSize / step);
    glUniform1i(terrain_depth_.uniform("uCells"), cells);
    glUniform1i(terrain_depth_.uniform("uWall"), 0);
    heights_.bind(5);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, chunk_buffer_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(caster_chunks.size() * sizeof(Vec2)), caster_chunks.data(), GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLES, 0, cells * cells * 6, static_cast<GLsizei>(caster_chunks.size()));

    const Vec2 right = camera.unrotate({IsometricCamera::kX, -IsometricCamera::kX});
    std::vector<ShadowCaster> casters;
    casters.reserve(decor_.size() / 2);
    for (const Decor& decor : decor_) {
        if (!decor.casts_shadow) continue;
        const SpriteInstance& sprite = decor.sprite;
        const f32 height = surface_height(decor.ground, camera.zoom);
        Vec2 lo{1.0e9f, 1.0e9f}, hi{-1.0e9f, -1.0e9f};
        for (i32 corner = 0; corner < 4; ++corner) {
            const Vec2 p = decor.ground + right * (sprite.x + ((corner & 1) ? sprite.w : 0.0f));
            const f32 z = height - (sprite.y + ((corner & 2) ? sprite.h * sprite.anchor : 0.0f)) / IsometricCamera::kZ;
            const Vec2 q = p - shadow_view_.light * z;
            lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y);
            hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y);
        }
        if (!intersects(lo, hi)) continue;
        casters.push_back({decor.ground.x, decor.ground.y, height, sprite.x, sprite.y, sprite.w, sprite.h,
                           sprite.u0, sprite.v0, sprite.u1, sprite.v1, sprite.anchor});
    }
    scenery_depth_.bind();
    bind_shadows(scenery_depth_, &shadow_view_);
    glUniform2f(scenery_depth_.uniform("uRight"), right.x, right.y);
    glUniform1i(scenery_depth_.uniform("uAtlas"), 0);
    sprites_.bind(0);
    glBindVertexArray(caster_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, caster_buffer_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(casters.size() * sizeof(ShadowCaster)), casters.data(), GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(casters.size()));
    glBindVertexArray(0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    shadows_valid_ = true;
}

void MapRenderer::draw(const IsometricCamera& camera, const MapDrawOptions& options) {
    if (options.shadows) update_shadows(camera);
    glClearColor(0.030f, 0.036f, 0.046f, 1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);  // either side of steep relief can face the camera
    glDisable(GL_BLEND);
    terrain_.bind();
    bind_shadows(terrain_, options.shadows ? &shadow_view_ : nullptr);
    glUniform1i(terrain_.uniform("uShadowPass"), 0);
    const Camera2D plane = camera.plane_camera();
    const Vec2 scale = plane.clip_scale(), offset = plane.clip_offset();
    glUniform4f(u_view_, scale.x, scale.y, offset.x, offset.y);
    const Vec2 rx = camera.rotate({1, 0}), ry = camera.rotate({0, 1});
    const f32 rotation[] = {rx.x, rx.y, ry.x, ry.y};
    glUniformMatrix2fv(u_rotation_, 1, GL_FALSE, rotation);
    const f32 depth_scale = 1.0f / (static_cast<f32>(map_width_ + map_height_) + height_range_ * tiles_per_unit_ + 32.0f);
    glUniform1f(u_depth_scale_, depth_scale);
    glUniform2f(u_map_size_, static_cast<f32>(map_width_), static_cast<f32>(map_height_));
    glUniform1f(u_time_, options.time);
    glUniform1f(u_grid_, options.grid ? 1.0f : 0.0f);
    glUniform1i(u_overlay_, static_cast<int>(options.overlay));
    glUniform1f(u_pixel_, 1.0f / camera.zoom);
    glUniform1f(u_unit_, tiles_per_unit_);
    tiles_.bind(0);
    relief_.bind(1);
    depth_.bind(2);
    ground_.bind(3);
    surface_.bind(4);
    heights_.bind(5);

    // All visible chunks share a tessellation level, so their edges match.
    // Distant views avoid submitting millions of subpixel triangles.
    const f32 step = mesh_step(camera.zoom);
    const i32 cells = static_cast<i32>(kChunkSize / step);
    glUniform1f(u_step_, step);
    glUniform1i(u_cells_, cells);
    glUniform1i(u_wall_, 0);
    visible_chunks_.clear();
    for (const Chunk& chunk : chunks_) {
        Vec2 lo{1.0e9f, 1.0e9f}, hi{-1.0e9f, -1.0e9f};
        for (i32 corner = 0; corner < 4; ++corner) {
            const Vec2 p = chunk.origin + Vec2{static_cast<f32>(corner & 1) * kChunkSize, static_cast<f32>(corner >> 1) * kChunkSize};
            for (f32 z : {chunk.low, chunk.high}) {
                const Vec2 q = camera.world_to_screen(p, z);
                lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y);
                hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y);
            }
        }
        if (hi.x < -2 || hi.y < -2 || lo.x > camera.viewport.x + 2 || lo.y > camera.viewport.y + 2) continue;
        visible_chunks_.push_back(chunk.origin);
    }
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, chunk_buffer_);
    // One spare entry also makes the border draw valid when no chunks are visible.
    if (visible_chunks_.empty()) visible_chunks_.push_back({0, 0});
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(visible_chunks_.size() * sizeof(Vec2)), visible_chunks_.data(), GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLES, 0, cells * cells * 6, static_cast<GLsizei>(visible_chunks_.size()));
    glUniform1i(u_wall_, 1);
    const i32 edge_segments = static_cast<i32>(std::ceil(static_cast<f32>(map_width_) / step) + std::ceil(static_cast<f32>(map_height_) / step)) * 2;
    glDrawArraysInstanced(GL_TRIANGLES, 0, edge_segments * 6, 1);

    sprites_drawn_ = 0;
    if (camera.zoom >= kDecorMinZoom && !decor_.empty()) {
        SpriteInstance* out = batch_.begin(static_cast<u32>(decor_.size()));
        const auto& order = decor_order_[(camera.rotation % 4 + 4) % 4];
        for (const u32 k : order) {
            const Decor& decor = decor_[k];
            Vec2 projected = camera.project(decor.ground, decor.height);
            SpriteInstance sprite = decor.sprite;
            sprite.x += projected.x;
            sprite.y += projected.y;
            const Vec2 screen = (Vec2{sprite.x, sprite.y} - plane.center) * camera.zoom + camera.viewport * 0.5f;
            if (screen.x + sprite.w * camera.zoom < -4 || screen.y + sprite.h * camera.zoom < -4 || screen.x > camera.viewport.x + 4 || screen.y > camera.viewport.y + 4) continue;
            const f32 height = surface_height(decor.ground, camera.zoom);
            projected = camera.project(decor.ground, height);
            sprite.y = decor.sprite.y + projected.y;
            sprite.depth = -camera.depth(decor.ground, height) * depth_scale - 0.025f * depth_scale;
            sprite.depth_span = sprite.h / (2.0f * IsometricCamera::kZ) * depth_scale;
            sprite.ground_span = -sprite.h * (IsometricCamera::kDepthXY / IsometricCamera::kY) * depth_scale;
            out[sprites_drawn_++] = sprite;
        }
        // Sort back to front, retain transparent edges, and compare every pixel
        // with terrain. Transparent canvas pixels never occlude another object.
        glDepthMask(GL_FALSE);
        batch_.draw(sprites_drawn_, plane, sprites_, options.shadows ? &shadow_view_ : nullptr, &camera, depth_scale);
        glDepthMask(GL_TRUE);
    }
    glDisable(GL_DEPTH_TEST);
}

}  // namespace zl
