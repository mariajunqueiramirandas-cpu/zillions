#include "world/scenery.h"

#include <algorithm>
#include <cmath>

#include "core/rng.h"
#include "math/noise.h"
#include "world/tilemap.h"

namespace zl {
namespace {
Color tint(f32 brightness, f32 warmth) {
    const auto channel = [](f32 v) { return static_cast<u8>(saturate(v) * 255.0f + 0.5f); };
    return {channel(brightness * (1.0f + warmth)), channel(brightness), channel(brightness * (1.0f - warmth)), 255};
}

f32 distance_squared(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const f32 square = dot(ab, ab);
    const f32 t = square > 1.0e-12f ? saturate(dot(p - a, ab) / square) : 0.0f;
    const Vec2 delta = p - (a + ab * t);
    return dot(delta, delta);
}

bool vertical_overlap(const ObjectCollider& collider, f32 bottom, f32 height) {
    return bottom < collider.top - 1.0e-5f && bottom + height > collider.bottom + 1.0e-5f;
}

bool capsule_hit(Vec2 from, Vec2 delta, Vec2 a, Vec2 b, f32 radius, f32& time, Vec2& normal) {
    bool found = false;
    const f32 speed_squared = dot(delta, delta);
    if (speed_squared < 1.0e-14f) return false;
    const auto accept = [&](f32 t, Vec2 n) {
        if (t < -1.0e-6f || t > time || dot(delta, n) >= -1.0e-8f) return;
        time = std::max(t, 0.0f); normal = n; found = true;
    };
    // Endcaps: the exact quadratic time of impact, not a sampled endpoint.
    for (const Vec2 centre : {a, b}) {
        const Vec2 offset = from - centre;
        const f32 along = dot(offset, delta);
        const f32 c = dot(offset, offset) - radius * radius;
        const f32 discriminant = along * along - speed_squared * c;
        if (discriminant < 0.0f) continue;
        const f32 t = (-along - std::sqrt(discriminant)) / speed_squared;
        Vec2 n = from + delta * std::max(0.0f, t) - centre;
        const f32 length_n = length(n);
        if (length_n > 1.0e-7f) accept(c <= 0.0f ? 0.0f : t, n / length_n);
    }
    // The straight sides of the capsule.
    const Vec2 ab = b - a;
    const f32 extent = length(ab);
    if (extent > 1.0e-7f) {
        const Vec2 axis = ab / extent;
        for (const f32 sign : {-1.0f, 1.0f}) {
            const Vec2 n{-axis.y * sign, axis.x * sign};
            const f32 speed = dot(delta, n);
            if (speed >= -1.0e-8f) continue;
            const f32 t = (radius - dot(from - a, n)) / speed;
            const f32 along = dot(from + delta * std::max(t, 0.0f) - a, axis);
            if (along >= 0.0f && along <= extent) accept(t, n);
        }
    }
    return found;
}
}  // namespace

bool scenery_blocks(SceneryKind kind) {
    return kind == SceneryKind::Broadleaf || kind == SceneryKind::Conifer ||
           kind == SceneryKind::Boulder || kind == SceneryKind::Stone ||
           kind == SceneryKind::Iron || kind == SceneryKind::Gold || kind == SceneryKind::Chest;
}

f32 collision_floor(const TileMap& map, Vec2 position) {
    const i32 x = static_cast<i32>(std::floor(position.x)), y = static_cast<i32>(std::floor(position.y));
    if (!map.in_bounds(x, y) || map.level.empty()) return 0.0f;
    const auto tile = [&](i32 tx, i32 ty) { return static_cast<size_t>(map.index(clamp(tx, 0, map.width - 1), clamp(ty, 0, map.height - 1))); };
    const f32 qx = position.x - 0.5f, qy = position.y - 0.5f;
    const i32 ix = static_cast<i32>(std::floor(qx)), iy = static_cast<i32>(std::floor(qy));
    const size_t neighbours[] = {tile(ix, iy), tile(ix + 1, iy), tile(ix, iy + 1), tile(ix + 1, iy + 1)};
    bool ramp = false;
    for (size_t index : neighbours) ramp = ramp || (map.flags[index] & TileFlag::Ramp) != 0;
    f32 level = static_cast<f32>(map.level[tile(x, y)]);
    if (ramp) {
        // A ramp has a continuous floor, not a one-level jump when the body's
        // centre crosses a tile border. Match the relief's ramp control nodes.
        const auto node = [&](i32 tx, i32 ty) {
            const size_t index = tile(tx, ty);
            const u8 centre = map.level[index];
            f32 value = static_cast<f32>(centre);
            if (map.flags[index] & TileFlag::Ramp) {
                for (const IVec2 d : {IVec2{1, 0}, IVec2{-1, 0}, IVec2{0, 1}, IVec2{0, -1}}) {
                    const size_t neighbour = tile(tx + d.x, ty + d.y);
                    if (map.terrain[neighbour] == Terrain::Mountain) continue;
                    const u8 other = map.level[neighbour];
                    if (other > centre) value = static_cast<f32>(centre) + 0.3f;
                    if (other < centre) value = static_cast<f32>(centre) - 0.3f;
                }
            }
            return value;
        };
        const f32 fx = qx - static_cast<f32>(ix), fy = qy - static_cast<f32>(iy);
        level = lerp(lerp(node(ix, iy), node(ix + 1, iy), fx), lerp(node(ix, iy + 1), node(ix + 1, iy + 1), fx), fy);
    }
    return level * kTerrainLevelHeight * static_cast<f32>(map.resolution);
}

namespace {
void build_colliders(Scenery& scene, const TileMap& map) {
    scene.colliders.clear();
    for (u32 i = 0; i < static_cast<u32>(scene.objects.size()); ++i) {
        const SceneryObject& object = scene.objects[i];
        if (!object.active || !scenery_blocks(object.kind)) continue;
        const bool tree = object.kind == SceneryKind::Broadleaf || object.kind == SceneryKind::Conifer;
        f32 radius = object.kind == SceneryKind::Conifer ? 0.12f : 0.15f;
        f32 half_length = 0.0f, height = object.kind == SceneryKind::Conifer ? 2.5f : 2.3f;
        if (!tree) {
            const bool boulder = object.kind == SceneryKind::Boulder;
            const u32 variant = object.variant % (boulder ? 4u : 3u);
            radius = (boulder ? 0.35f : 0.32f) + 0.02f * static_cast<f32>(variant % 2);
            half_length = (boulder && variant >= 2) ? 0.24f : 0.18f;
            height = boulder ? 0.85f : 0.70f;
        }
        if (object.kind == SceneryKind::Chest) { radius = 0.33f; half_length = 0.18f; height = 0.65f; }
        const Vec2 extent=object.facing%2 ? Vec2{0,half_length*object.scale} : Vec2{half_length*object.scale,0};
        const f32 bottom = collision_floor(map, object.position);
        scene.colliders.push_back({object.position - extent, object.position + extent, radius * object.scale,
                                   bottom, bottom + height * object.scale, i});
    }
    scene.rebuild_index(map.width, map.height);
}

} // namespace

Scenery build_scenery(const TileMap& map) {
    Scenery scene;
    if (map.width <= 0 || map.height <= 0 || map.terrain.size() != static_cast<size_t>(map.tile_count())) return scene;
    scene.objects.reserve(static_cast<size_t>(map.tile_count() / 3));
    const u32 seed = static_cast<u32>(mix64(map.seed));
    const auto resource = [&](size_t i) { return i < map.resource.size() ? map.resource[i] : Resource::None; };
    const auto elevation = [&](size_t i) { return i < map.elevation.size() ? map.elevation[i] : map.sea_level + 0.2f; };
    const auto moisture = [&](size_t i) { return i < map.moisture.size() ? map.moisture[i] : 0.5f; };
    const auto add = [&](SceneryKind kind, u32 variant, f32 x, f32 y, f32 scale, Color color) {
        scene.objects.push_back({kind, variant, {x, y}, scale, color, map.index(static_cast<i32>(x), static_cast<i32>(y))});
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

            if (resource(i) != Resource::None) {
                const SceneryKind set = resource(i) == Resource::Stone ? SceneryKind::Stone : (resource(i) == Resource::Iron ? SceneryKind::Iron : SceneryKind::Gold);
                add(set, h3, cx + (r0 - 0.5f) * 0.2f, cy + 0.2f + (r1 - 0.5f) * 0.2f, 0.95f + 0.25f * r2, tint(0.94f + 0.06f * r3, 0.0f));
                continue;
            }

            switch (map.terrain[i]) {
                case Terrain::Forest: {
                    // Conifers take over on higher, drier ground.
                    const f32 altitude = (elevation(i) - map.sea_level) / band;
                    const f32 conifer = smoothstep(0.45f, 0.80f, altitude + (0.5f - moisture(i)) * 0.5f);
                    const int trees = r3 < 0.40f ? 2 : 1;
                    for (int k = 0; k < trees; ++k) {
                        const u32 hk = hash32(h3 + static_cast<u32>(k) * 0x9E3779B9u);
                        const f32 a = hash_to_unit(hk), b = hash_to_unit(hash32(hk + 1)), c = hash_to_unit(hash32(hk + 2)), d = hash_to_unit(hash32(hk + 3));
                        const bool is_conifer = hash_to_unit(hash32(hk + 4)) < conifer;
                        add(is_conifer ? SceneryKind::Conifer : SceneryKind::Broadleaf, hash32(hk + 5), cx + (a - 0.5f) * 0.9f, cy + (b - 0.5f) * 0.9f,
                            0.80f + 0.35f * c, tint(0.84f + 0.16f * d, (hash_to_unit(hash32(hk + 6)) - 0.5f) * 0.10f));
                    }
                    break;
                }
                case Terrain::Rocks: {
                    add(SceneryKind::Boulder, h3, cx + (r0 - 0.5f) * 0.5f, cy + 0.25f + (r1 - 0.5f) * 0.4f, 0.85f + 0.45f * r2, tint(0.88f + 0.12f * r3, 0.02f));
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
                    if (waterside && r0 < 0.45f) add(SceneryKind::Reed, h3, gx, gy, 0.9f + 0.3f * r3, tint(0.95f, 0.0f));
                    else if (r0 < shrubs) add(SceneryKind::Bush, h3, gx, gy, 0.9f + 0.4f * r3, tint(0.9f + 0.1f * r3, 0.0f));
                    else if (r0 < shrubs + flowers) add(SceneryKind::Flower, h3, gx, gy, 0.9f + 0.3f * r3, tint(1.0f, 0.0f));
                    else if (r0 < shrubs + flowers + grass) add(SceneryKind::Tuft, h3, gx, gy, 0.8f + 0.4f * r3, tint(0.92f + 0.08f * r3, 0.0f));
                    break;
                }
                case Terrain::Dirt: {
                    const f32 stones = 0.04f + 0.22f * smoothstep(0.30f, 0.80f, simplex2(cx / 7.0f, cy / 7.0f, seed + 4u));
                    if (r0 < stones) add(SceneryKind::Pebble, h3, cx + (r1 - 0.5f) * 0.8f, cy + (r2 - 0.5f) * 0.8f, 0.8f + 0.5f * r3, tint(0.95f, 0.04f));
                    else if (r0 < stones + 0.05f) add(SceneryKind::Tuft, h3, cx + (r1 - 0.5f) * 0.8f, cy + (r2 - 0.5f) * 0.8f, 0.8f + 0.3f * r3, tint(0.9f, 0.08f));
                    break;
                }
                case Terrain::Sand:
                    if (r0 < 0.03f) add(SceneryKind::Pebble, h3, cx + (r1 - 0.5f) * 0.8f, cy + (r2 - 0.5f) * 0.8f, 0.7f + 0.4f * r3, tint(1.0f, 0.03f));
                    break;
                case Terrain::Shallows:
                    if (r0 < 0.16f) add(SceneryKind::Pebble, h3, cx + (r1 - 0.5f) * 0.8f, cy + (r2 - 0.5f) * 0.8f, 0.9f + 0.5f * r3, tint(0.9f, -0.04f));
                    break;
                default: break;
            }
        }
    }

    build_colliders(scene, map);
    return scene;
}

void rebuild_scenery_colliders(TileMap& map) { build_colliders(map.scenery, map); }

void Scenery::cells(Vec2 low, Vec2 high, i32& x0, i32& y0, i32& x1, i32& y1) const {
    x0 = clamp(static_cast<i32>(std::floor(low.x / kCellSize)), 0, columns_ - 1);
    y0 = clamp(static_cast<i32>(std::floor(low.y / kCellSize)), 0, rows_ - 1);
    x1 = clamp(static_cast<i32>(std::floor(high.x / kCellSize)), 0, columns_ - 1);
    y1 = clamp(static_cast<i32>(std::floor(high.y / kCellSize)), 0, rows_ - 1);
}

void Scenery::rebuild_index(i32 width, i32 height) {
    ready = false;
    columns_ = static_cast<i32>(std::ceil(static_cast<f32>(width) / kCellSize));
    rows_ = static_cast<i32>(std::ceil(static_cast<f32>(height) / kCellSize));
    if (columns_ <= 0 || rows_ <= 0) return;
    offsets_.assign(static_cast<size_t>(columns_ * rows_) + 1, 0);
    const auto visit = [&](const ObjectCollider& collider, const auto& visitor) {
        const Vec2 radius{collider.radius, collider.radius};
        i32 x0, y0, x1, y1;
        cells({std::min(collider.a.x, collider.b.x) - radius.x, std::min(collider.a.y, collider.b.y) - radius.y},
              {std::max(collider.a.x, collider.b.x) + radius.x, std::max(collider.a.y, collider.b.y) + radius.y}, x0, y0, x1, y1);
        for (i32 y = y0; y <= y1; ++y) for (i32 x = x0; x <= x1; ++x) visitor(static_cast<size_t>(y * columns_ + x));
    };
    for (const auto& collider : colliders) visit(collider, [&](size_t cell) { ++offsets_[cell + 1]; });
    for (size_t i = 1; i < offsets_.size(); ++i) offsets_[i] += offsets_[i - 1];
    indices_.resize(offsets_.back());
    auto cursor = offsets_;
    for (u32 i = 0; i < static_cast<u32>(colliders.size()); ++i) {
        visit(colliders[i], [&](size_t cell) { indices_[cursor[cell]++] = i; });
    }
    ready = true;
}

bool Scenery::overlaps(Vec2 position, f32 radius, f32 bottom, f32 height) const {
    if (!ready || colliders.empty()) return false;
    i32 x0, y0, x1, y1;
    cells(position - Vec2{radius, radius}, position + Vec2{radius, radius}, x0, y0, x1, y1);
    for (i32 y = y0; y <= y1; ++y) for (i32 x = x0; x <= x1; ++x) {
        const size_t cell = static_cast<size_t>(y * columns_ + x);
        for (u32 i = offsets_[cell]; i < offsets_[cell + 1]; ++i) {
            const ObjectCollider& collider = colliders[indices_[i]];
            const f32 reach = radius + collider.radius;
            if (vertical_overlap(collider, bottom, height) && distance_squared(position, collider.a, collider.b) < reach * reach - 1.0e-7f) return true;
        }
    }
    return false;
}

bool Scenery::sweep(Vec2 from, Vec2 displacement, f32 radius, f32 bottom, f32 height, ObjectContact& hit) const {
    hit.time = 1.0f;
    if (!ready || colliders.empty() || dot(displacement, displacement) < 1.0e-14f) return false;
    const Vec2 to = from + displacement;
    i32 x0, y0, x1, y1;
    cells({std::min(from.x, to.x) - radius, std::min(from.y, to.y) - radius},
          {std::max(from.x, to.x) + radius, std::max(from.y, to.y) + radius}, x0, y0, x1, y1);
    bool found = false;
    for (i32 y = y0; y <= y1; ++y) for (i32 x = x0; x <= x1; ++x) {
        const size_t cell = static_cast<size_t>(y * columns_ + x);
        for (u32 i = offsets_[cell]; i < offsets_[cell + 1]; ++i) {
            const u32 index = indices_[i];
            const ObjectCollider& collider = colliders[index];
            if (!vertical_overlap(collider, bottom, height)) continue;
            if (capsule_hit(from, displacement, collider.a, collider.b, radius + collider.radius, hit.time, hit.normal)) {
                hit.collider = index; found = true;
            }
        }
    }
    return found;
}

Vec2 Scenery::slide(Vec2 from, Vec2 displacement, f32 radius, f32 bottom, f32 height) const {
    Vec2 position = from, remaining = displacement;
    for (i32 iteration = 0; iteration < 5; ++iteration) {
        ObjectContact hit;
        if (!sweep(position, remaining, radius, bottom, height, hit)) return position + remaining;
        const f32 distance = length(remaining);
        const f32 time = std::max(0.0f, hit.time - 0.0001f / std::max(distance, 0.0001f));
        position += remaining * time;
        remaining *= 1.0f - time;
        remaining -= hit.normal * std::min(dot(remaining, hit.normal), 0.0f);
        if (dot(remaining, remaining) < 1.0e-12f) break;
    }
    return position;
}

}  // namespace zl
