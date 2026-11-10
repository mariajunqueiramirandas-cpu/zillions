// Headless world-space collision regression tests.
#include <cmath>
#include <cstdio>
#include <limits>

#include "actor/player.h"
#include "core/jobs.h"
#include "gfx/isometric_camera.h"
#include "world/mapgen.h"

using namespace zl;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

TileMap flat_map() {
    TileMap map;
    map.width = map.height = 32;
    map.start = {5, 8};
    map.seed = 7;
    map.terrain.assign(1024, Terrain::Dirt);
    map.flags.assign(1024, TileFlag::Walkable | TileFlag::Buildable);
    map.level.assign(1024, 0);
    map.resource.assign(1024, Resource::None);
    map.elevation.assign(1024, 0.1f);
    map.moisture.assign(1024, 0.5f);
    return map;
}

void continuous_contacts() {
    Scenery scene;
    scene.colliders.push_back({{8, 8}, {8, 8}, 0.2f, 0, 2.4f, 0});
    scene.rebuild_index(32, 32);
    for (int angle = 0; angle < 32; ++angle) {
        const f32 a = static_cast<f32>(angle) * 2.0f * kPi / 32.0f;
        const Vec2 direction{std::cos(a), std::sin(a)};
        const Vec2 start = Vec2{8, 8} - direction * 5;
        ObjectContact hit;
        CHECK(scene.sweep(start, direction * 10, 0.26f, 0, 1.4f, hit));
        CHECK(std::fabs(hit.time - 0.454f) < 0.00002f);
        CHECK(dot(hit.normal, direction) < -0.9999f);
        CHECK(!scene.overlaps(start + direction * (hit.time * 10 - 0.001f), 0.26f, 0, 1.4f));
        CHECK(scene.overlaps(start + direction * (hit.time * 10 + 0.001f), 0.26f, 0, 1.4f));
        const Vec2 stopped = scene.slide(start, direction * 100, 0.26f, 0, 1.4f);
        CHECK(!scene.overlaps(stopped, 0.26f, 0, 1.4f));
        CHECK(length(stopped - Vec2{8, 8}) < 0.461f);
    }
    ObjectContact hit;
    CHECK(!scene.sweep({4, 8}, {10, 0}, 0.26f, 2.4f, 1.4f, hit));
    CHECK(!scene.overlaps({8, 8}, 0.26f, -2, 1.4f));
    CHECK(scene.overlaps({8, 8}, 0.26f, 2.399f, 1.4f));
    CHECK(!scene.sweep({4, 8.461f}, {10, 0}, 0.26f, 0, 1.4f, hit));

    scene.colliders = {{{8, 8}, {10, 8}, 0.4f, 0, 0.8f, 0}};
    scene.rebuild_index(32, 32);
    CHECK(scene.sweep({9, 5}, {0, 10}, 0.26f, 0, 1.4f, hit));
    CHECK(std::fabs(hit.time - 0.234f) < 0.00002f);
    CHECK(hit.normal.y < -0.999f);
    CHECK(scene.sweep({4, 8}, {20, 0}, 0.26f, 0, 1.4f, hit));
    CHECK(std::fabs(hit.time - 0.167f) < 0.00002f);
    CHECK(!scene.overlaps({9, 8}, 0.26f, 0.8f, 1.4f));
}

void sliding_and_passages() {
    Scenery scene;
    scene.colliders = {{{8, 8}, {8, 8}, 0.4f, 0, 2.0f, 0}};
    scene.rebuild_index(32, 32);
    for (f32 side : {-1.0f, 1.0f}) {
        const Vec2 start{6.5f, 8 + side * 0.3f};
        const Vec2 result = scene.slide(start, {3, side}, 0.26f, 0, 1.4f);
        CHECK(!scene.overlaps(result, 0.26f, 0, 1.4f));
        CHECK(result.x > start.x + 1.0f);
        CHECK((result.y - start.y) * side > 0.5f);
    }
    for (f32 separation : {1.0f, 1.3f}) {
        scene.colliders = {{{8, 8 - separation * 0.5f}, {8, 8 - separation * 0.5f}, 0.3f, 0, 2, 0},
                           {{8, 8 + separation * 0.5f}, {8, 8 + separation * 0.5f}, 0.3f, 0, 2, 1}};
        scene.rebuild_index(32, 32);
        const Vec2 end = scene.slide({6, 8}, {4, 0}, 0.26f, 0, 1.4f);
        CHECK(!scene.overlaps(end, 0.26f, 0, 1.4f));
        if (separation < 1.1f) CHECK(end.x < 8);
        else CHECK(std::fabs(end.x - 10) < 0.0001f);
        const Vec2 away = scene.slide(end, {-2, 0}, 0.26f, 0, 1.4f);
        CHECK(away.x < end.x - 1.9f);
        CHECK(!scene.overlaps(away, 0.26f, 0, 1.4f));
    }
}

void generated_footprints_and_spawn() {
    TileMap map = flat_map();
    const size_t tile = static_cast<size_t>(map.index(8, 8));
    map.terrain[tile] = Terrain::Forest;
    map.flags[tile] = 0;
    map.scenery = build_scenery(map);
    CHECK(map.scenery.ready && !map.scenery.colliders.empty());
    int free = 0, blocked = 0;
    for (f32 y = 8.05f; y < 9; y += 0.1f) for (f32 x = 8.05f; x < 9; x += 0.1f) {
        if (can_stand(map, {x, y}, Player::kRadius)) ++free;
        else ++blocked;
    }
    CHECK(free > 0 && blocked > 0); // The trunk blocks, not the whole forest tile.
    for (const ObjectCollider& collider : map.scenery.colliders) {
        const SceneryObject& object = map.scenery.objects[collider.object];
        CHECK(length((collider.a + collider.b) * 0.5f - object.position) < 0.00001f);
        CHECK(scenery_blocks(object.kind));
        CHECK(!can_stand(map, object.position, Player::kRadius));
        Player player;
        CHECK(player.spawn(map, object.position));
        CHECK(can_stand(map, player.position(), player.body_radius(), player.body_height()));
    }
    for (const Resource resource : {Resource::Stone, Resource::Iron, Resource::Gold}) {
        TileMap ore = flat_map();
        ore.resource[tile] = resource;
        ore.flags[tile] = 0;
        ore.scenery = build_scenery(ore);
        CHECK(ore.scenery.colliders.size() == 1);
        const auto& collider = ore.scenery.colliders[0];
        CHECK(!can_stand(ore, (collider.a + collider.b) * 0.5f, Player::kRadius));
    }
    map = flat_map();
    map.terrain.assign(1024, Terrain::Grass);
    map.scenery = build_scenery(map);
    CHECK(map.scenery.colliders.empty());
    CHECK(!map.scenery.objects.empty());
    for (const SceneryObject& object : map.scenery.objects) {
        if (object.position.x > 1 && object.position.y > 1 && object.position.x < 31 && object.position.y < 31)
            CHECK(can_stand(map, object.position, Player::kRadius));
    }
    CHECK(!can_stand(map, {std::numeric_limits<f32>::quiet_NaN(), 8}, 0.26f));
    CHECK(!can_stand(map, {8, 8}, -1));
}

void player_contacts_at_every_angle() {
    TileMap map = flat_map();
    map.scenery.colliders = {{{8.5f, 8.5f}, {8.5f, 8.5f}, 0.2f, 0, 2.4f, 0}};
    map.scenery.rebuild_index(map.width, map.height);
    Vec2 reference;
    for (int rotation = 0; rotation < 4; ++rotation) {
        IsometricCamera camera;
        camera.rotation = rotation;
        camera.zoom = 64;
        const Vec2 screen = camera.project({1, 0}) * camera.zoom;
        const Vec2 move = camera.screen_delta_to_world(screen);
        Player player;
        CHECK(player.spawn(map, {6, 8.5f}));
        for (int tick = 0; tick < 300; ++tick) {
            player.step(map, {move, true, false}, 1.0f / 60.0f);
            CHECK(can_stand(map, player.position(), player.body_radius(), player.body_height()));
        }
        CHECK(player.position().x < 8.041f && player.position().x > 8.02f);
        CHECK(player.motion().speed < 0.01f);
        if (rotation == 0) reference = player.position();
        else CHECK(length(player.position() - reference) < 0.00001f);
        for (int tick = 0; tick < 60; ++tick) player.step(map, {{-1, 0}, false, false}, 1.0f / 60.0f);
        CHECK(player.position().x < 7);
    }
    Player fast;
    CHECK(fast.spawn(map, {6, 8.5f}));
    fast.step(map, {{1, 0}, true, false}, 2.0f);
    CHECK(fast.position().x < 8.041f);
    CHECK(can_stand(map, fast.position(), fast.body_radius()));
    for (const Vec2 size : {Vec2{0.85f, 0.8f}, Vec2{1.15f, 1.2f}}) {
        Player scaled;
        scaled.look.height = size.x; scaled.look.build = size.y;
        CHECK(scaled.spawn(map, {6, 8.5f}));
        for (int tick = 0; tick < 300; ++tick) scaled.step(map, {{1, 0}, true, false}, 1.0f / 60.0f);
        CHECK(std::fabs(length(scaled.position() - Vec2{8.5f, 8.5f}) - (0.2f + scaled.body_radius())) < 0.001f);
        CHECK(can_stand(map, scaled.position(), scaled.body_radius(), scaled.body_height()));
    }
    // An overhead collider has real height: a normal character fits below it,
    // while a taller body cannot occupy the same horizontal footprint.
    map.scenery.colliders = {{{8.5f, 8.5f}, {8.5f, 8.5f}, 0.3f, 1.5f, 3.0f, 0}};
    map.scenery.rebuild_index(32, 32);
    CHECK(can_stand(map, {8.5f, 8.5f}, 0.26f, 1.4f));
    CHECK(!can_stand(map, {8.5f, 8.5f}, 0.26f, 1.7f));

    // Ramps raise the whole body continuously; the head must not pass through
    // an object on the upper terrace while the centre remains in the low tile.
    map = flat_map();
    for (int y = 0; y < 32; ++y) for (int x = 8; x < 32; ++x) map.level[static_cast<size_t>(map.index(x, y))] = 1;
    map.flags[static_cast<size_t>(map.index(7, 8))] |= TileFlag::Ramp;
    map.flags[static_cast<size_t>(map.index(8, 8))] |= TileFlag::Ramp;
    CHECK(collision_floor(map, {7.2f, 8.5f}) < collision_floor(map, {7.9f, 8.5f}));
    CHECK(collision_floor(map, {7.9f, 8.5f}) < collision_floor(map, {8.8f, 8.5f}));
    map.scenery.colliders = {{{7.9f, 8.5f}, {7.9f, 8.5f}, 0.15f, 1.6f, 2.5f, 0}};
    map.scenery.rebuild_index(32, 32);
    CHECK(!can_stand(map, {7.9f, 8.5f}, 0.26f, 1.4f));
    const f32 ramp_height = collision_floor(map, {8.5f, 8.5f});
    map.terrain[static_cast<size_t>(map.index(8, 7))] = Terrain::Mountain;
    map.level[static_cast<size_t>(map.index(8, 7))] = 2;
    CHECK(collision_floor(map, {8.5f, 8.5f}) == ramp_height);
}

void deterministic_layout_and_autonomy() {
    JobSystem single(1), parallel(4);
    MapGenParams params;
    params.size = 128;
    params.resolution = 1;
    params.seed = 17;
    const TileMap a = generate_map(params, single), b = generate_map(params, parallel);
    CHECK(a.scenery.objects.size() == b.scenery.objects.size());
    CHECK(a.scenery.colliders.size() == b.scenery.colliders.size());
    for (size_t i = 0; i < a.scenery.objects.size(); ++i) {
        const auto& x = a.scenery.objects[i]; const auto& y = b.scenery.objects[i];
        CHECK(x.kind == y.kind && x.variant == y.variant && x.position.x == y.position.x && x.position.y == y.position.y && x.scale == y.scale);
    }
    TileMap map = flat_map();
    for (int y = 9; y < 23; ++y) {
        map.terrain[static_cast<size_t>(map.index(13, y))] = Terrain::Forest;
        map.flags[static_cast<size_t>(map.index(13, y))] = 0;
    }
    map.scenery = build_scenery(map);
    Player p, q;
    CHECK(p.spawn(map, {12, 16})); CHECK(q.spawn(map, {12, 16}));
    PlayerAutonomy ai, bi;
    ai.reset(p.position(), 42); bi.reset(q.position(), 42);
    f32 distance = 0;
    for (int tick = 0; tick < 1800; ++tick) {
        const Vec2 before = p.position();
        p.step(map, ai.step(map, p, 1.0f / 60.0f), 1.0f / 60.0f);
        q.step(map, bi.step(map, q, 1.0f / 60.0f), 1.0f / 60.0f);
        CHECK(can_stand(map, p.position(), p.body_radius(), p.body_height()));
        CHECK(p.position().x == q.position().x && p.position().y == q.position().y);
        distance += length(p.position() - before);
    }
    CHECK(distance > 8);
}
}  // namespace

int main() {
    continuous_contacts();
    sliding_and_passages();
    generated_footprints_and_spawn();
    player_contacts_at_every_angle();
    deterministic_layout_and_autonomy();
    std::printf("world object collision checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
