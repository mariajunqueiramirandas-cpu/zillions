#include "render/map_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/rng.h"
#include "gfx/gl.h"
#include "math/noise.h"
#include "render/relief.h"

namespace zl {

namespace {

// Below this many pixels per tile, scenery sprites are smaller than a couple
// of pixels and the ground shader alone carries the picture.
constexpr f32 kDecorMinZoom = 2.5f;

// The terrain shader hard-codes the texture-array layer that holds noise.
static_assert(kNoiseLayer == 8, "update NOISE in the terrain shader");

const char* kTerrainVertex = R"(#version 330 core
void main() {
    // One oversized triangle covers the screen; no vertex buffer needed.
    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);
}
)";

// The whole ground is drawn by this one shader, whatever the map size: each
// pixel finds its four nearest tiles in a data texture and blends their
// materials, so cost depends only on screen resolution.
const char* kTerrainFragment = R"(#version 330 core
uniform sampler2D uTiles;         // r: terrain id, g: resource id, b: tile flags, a: height level (point sampled)
uniform sampler2D uRelief;        // rg: surface normal, b: height, a: cavity (ridge > 0.5 > valley)
uniform sampler2D uSurface;       // r: mountain altitude, g: cliff face, b: height level
uniform sampler2D uDepth;         // r: distance from water to shore, g: moisture
uniform sampler2DArray uGround;   // one tiling texture per terrain, then noise
uniform vec2 uMapSize;
uniform vec4 uWorld;              // world = gl_FragCoord.xy * xy + zw
uniform float uTime;
uniform float uGrid;
uniform int uOverlay;             // 0: none, 1: height levels, 2: passability
uniform float uPixel;             // world units per screen pixel
uniform float uHeightRange;       // world units of height that relief.b == 1 stands for
uniform float uUnit;              // tiles per world unit
out vec4 oColor;

const float WATER = 0.0, SHALLOWS = 1.0, GRASS = 3.0, FOREST = 5.0, MOUNTAIN = 6.0, NOISE = 8.0;
const float GROUND_SCALE = 1.0 / 8.0;
const vec3 LIGHT = vec3(-0.5013, -0.6216, 0.6016);
const vec3 BACKDROP = vec3(0.030, 0.036, 0.046);
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
    vec2 p = gl_FragCoord.xy * uWorld.xy + uWorld.zw;
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
    // High ground towards the light throws a shadow: three steps of a ray march.
    // This is what makes a plateau read as raised: its far side drops into shade.
    vec2 sunward = normalize(LIGHT.xy) * uUnit / uMapSize;  // one world unit towards the light
    float rise1 = (texture(uRelief, mapUV + sunward * 0.6).b - relief.b) * uHeightRange;
    float rise2 = (texture(uRelief, mapUV + sunward * 1.5).b - relief.b) * uHeightRange;
    float rise3 = (texture(uRelief, mapUV + sunward * 2.8).b - relief.b) * uHeightRange;
    float shadow = max(max(smoothstep(0.25, 0.80, rise1), smoothstep(0.60, 1.40, rise2)), smoothstep(1.20, 2.30, rise3));
    // Ridges catch the light and valleys hold shade, beyond what the slope alone gives.
    float cavity = clamp((relief.a - 0.5) * 2.0, -1.0, 1.0);
    float cliff = surface.g;
    float shade = clamp(1.0 + (light - 1.0) * mix(0.42, 0.90, max(rock, cliff)), 0.69, 1.27);
    // On open ground the same term brightens the rim of a plateau and darkens the foot of its cliff.
    shade *= (1.0 - 0.38 * shadow) * (1.0 + cavity * mix(0.20, 0.28, rock));
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

    // An overcast, smoke-stained light over everything: a little colour
    // drained from the ground and a thin warm-grey haze laid on top.
    color = mix(vec3(dot(color, vec3(0.299, 0.587, 0.114))), color, 0.86);
    color = mix(color, HAZE, 0.06);

    vec2 fromEdge = abs(fract(p) - 0.5);
    float line = smoothstep(0.5 - 1.5 * uPixel, 0.5, max(fromEdge.x, fromEdge.y));
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

    // Fade to the backdrop just outside the map.
    vec2 outside = max(-p, p - uMapSize);
    float inside = 1.0 - smoothstep(0.0, 2.0 * uPixel, max(outside.x, outside.y));
    oColor = vec4(mix(BACKDROP, color, inside), 1.0);
}
)";

u32 tint(f32 brightness, f32 warmth) {
    const auto channel = [](f32 v) { return static_cast<u8>(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return pack_color(channel(brightness * (1.0f + warmth)), channel(brightness), channel(brightness * (1.0f - warmth)));
}

}  // namespace

MapRenderer::~MapRenderer() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void MapRenderer::init(JobSystem& jobs, const char* asset_dir) {
    art_ = load_terrain_art(jobs, asset_dir);
    ground_.create_array(art_.ground, TextureFilter::Trilinear, TextureWrap::Repeat);
    sprites_.create(art_.sprites, TextureFilter::Trilinear, TextureWrap::Clamp);
    // The CPU copies are only needed for upload.
    art_.ground.clear();
    art_.ground.shrink_to_fit();

    terrain_.compile("terrain", kTerrainVertex, kTerrainFragment);
    terrain_.bind();
    glUniform1i(terrain_.uniform("uTiles"), 0);
    glUniform1i(terrain_.uniform("uRelief"), 1);
    glUniform1i(terrain_.uniform("uDepth"), 2);
    glUniform1i(terrain_.uniform("uGround"), 3);
    glUniform1i(terrain_.uniform("uSurface"), 4);
    u_world_ = terrain_.uniform("uWorld");
    u_map_size_ = terrain_.uniform("uMapSize");
    u_time_ = terrain_.uniform("uTime");
    u_grid_ = terrain_.uniform("uGrid");
    u_overlay_ = terrain_.uniform("uOverlay");
    u_pixel_ = terrain_.uniform("uPixel");
    u_height_range_ = terrain_.uniform("uHeightRange");
    u_unit_ = terrain_.uniform("uUnit");

    glGenVertexArrays(1, &vao_);
    batch_.init();
}

void MapRenderer::set_map(const TileMap& map, JobSystem& jobs) {
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

    const Relief relief = build_relief(map, jobs);
    relief_.create(relief.shape, TextureFilter::Linear, TextureWrap::Clamp);
    surface_.create(relief.surface, TextureFilter::Linear, TextureWrap::Clamp);
    height_range_ = relief.height_range;
    tiles_per_unit_ = static_cast<f32>(map.resolution);

    build_decor(map);
}

void MapRenderer::build_decor(const TileMap& map) {
    struct Placed {
        f32 ground_y;
        SpriteInstance sprite;
    };
    std::vector<Placed> placed;
    placed.reserve(static_cast<size_t>(map.tile_count() / 3));
    const u32 seed = static_cast<u32>(mix64(map.seed));

    const auto add = [&](const SpriteSet& set, u32 pick, f32 gx, f32 gy, f32 scale, u32 color) {
        const SpriteDef& d = art_.defs[static_cast<size_t>(set.first + static_cast<i32>(pick % static_cast<u32>(set.count)))];
        SpriteInstance s;
        s.x = gx - d.anchor_x * scale;
        s.y = gy - d.anchor_y * scale;
        s.w = d.width * scale;
        s.h = d.height * scale;
        s.u0 = d.u0;
        s.v0 = d.v0;
        s.u1 = d.u1;
        s.v1 = d.v1;
        s.color = color;
        placed.push_back({gy, s});
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

    std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.ground_y < b.ground_y; });

    decor_.resize(placed.size());
    row_start_.assign(static_cast<size_t>(map.height) + 1, static_cast<u32>(placed.size()));
    i32 row = 0;
    for (size_t k = 0; k < placed.size(); ++k) {
        decor_[k] = placed[k].sprite;
        const i32 sprite_row = clamp(static_cast<i32>(std::floor(placed[k].ground_y)), 0, map.height - 1);
        for (; row <= sprite_row; ++row) row_start_[static_cast<size_t>(row)] = static_cast<u32>(k);
    }
}

void MapRenderer::draw(const Camera2D& camera, const MapDrawOptions& options) {
    // --- ground ---
    glDisable(GL_BLEND);
    terrain_.bind();
    const f32 inv_zoom = 1.0f / camera.zoom;
    glUniform4f(u_world_, inv_zoom, -inv_zoom, camera.center.x - camera.viewport.x * 0.5f * inv_zoom, camera.center.y + camera.viewport.y * 0.5f * inv_zoom);
    glUniform2f(u_map_size_, static_cast<f32>(map_width_), static_cast<f32>(map_height_));
    glUniform1f(u_time_, options.time);
    glUniform1f(u_grid_, options.grid ? 1.0f : 0.0f);
    glUniform1i(u_overlay_, static_cast<int>(options.overlay));
    glUniform1f(u_pixel_, inv_zoom);
    glUniform1f(u_height_range_, height_range_);
    glUniform1f(u_unit_, tiles_per_unit_);
    tiles_.bind(0);
    relief_.bind(1);
    depth_.bind(2);
    ground_.bind(3);
    surface_.bind(4);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // --- scenery ---
    sprites_drawn_ = 0;
    if (camera.zoom < kDecorMinZoom || decor_.empty()) return;
    const Vec2 lo = camera.visible_min(), hi = camera.visible_max();
    // Sprites rise above their ground row, so look a few rows further south.
    const i32 first_row = clamp(static_cast<i32>(std::floor(lo.y)) - 1, 0, map_height_);
    const i32 last_row = clamp(static_cast<i32>(std::floor(hi.y)) + 5, 0, map_height_);
    const u32 begin = row_start_[static_cast<size_t>(first_row)], end = row_start_[static_cast<size_t>(last_row)];
    if (end <= begin) return;

    SpriteInstance* out = batch_.begin(end - begin);
    u32 count = 0;
    if (lo.x <= 0.0f && hi.x >= static_cast<f32>(map_width_)) {
        count = end - begin;
        std::memcpy(out, decor_.data() + begin, static_cast<size_t>(count) * sizeof(SpriteInstance));
    } else {
        for (u32 k = begin; k < end; ++k) {
            const SpriteInstance& s = decor_[k];
            if (s.x > hi.x || s.x + s.w < lo.x) continue;
            out[count++] = s;
        }
    }
    batch_.draw(count, camera, sprites_);
    sprites_drawn_ = count;
}

}  // namespace zl
