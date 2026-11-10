// Headless test suite. A deliberately tiny harness: TEST registers a function,
// CHECK records a failure and carries on, main runs everything.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/image.h"
#include "core/jobs.h"
#include "core/rng.h"
#include "gfx/camera2d.h"
#include "math/noise.h"
#include "render/terrain_art.h"
#include "world/mapgen.h"

using namespace zl;

namespace {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int g_failures = 0;

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

#define TEST(name)                                        \
    void test_##name();                                   \
    const Registrar registrar_##name(#name, test_##name); \
    void test_##name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                        \
    } while (0)

// --- core ---------------------------------------------------------------------

TEST(rng_is_deterministic_and_in_range) {
    Rng a(42), b(42), c(43);
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        const u32 va = a.next_u32();
        CHECK(va == b.next_u32());
        differs = differs || va != c.next_u32();
    }
    CHECK(differs);
    Rng r(7);
    for (int i = 0; i < 10000; ++i) {
        const f32 f = r.next_f32();
        CHECK(f >= 0.0f && f < 1.0f);
        const i32 k = r.range_i(-3, 5);
        CHECK(k >= -3 && k <= 5);
        CHECK(r.below(10) < 10u);
    }
}

TEST(noise_is_bounded_and_varied) {
    f32 lo = 1.0e9f, hi = -1.0e9f;
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 300; ++x) {
            const f32 fx = static_cast<f32>(x) * 0.173f - 20.0f, fy = static_cast<f32>(y) * 0.173f - 20.0f;
            const f32 v = simplex2(fx, fy, 99u);
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
            CHECK(v == simplex2(fx, fy, 99u));
            const f32 r = ridged2(fx, fy, 5u, 4);
            CHECK(r >= 0.0f && r <= 1.0f);
        }
    }
    CHECK(lo >= -1.05f && hi <= 1.05f);
    CHECK(lo < -0.8f && hi > 0.8f);  // uses most of its range
    CHECK(simplex2(1.3f, 2.7f, 1u) != simplex2(1.3f, 2.7f, 2u));
}

TEST(parallel_for_visits_every_index_once) {
    JobSystem jobs(4);
    for (const i32 count : {1, 7, 64, 1000, 4097}) {
        std::vector<std::atomic<int>> hits(static_cast<size_t>(count));
        jobs.parallel_for(count, 16, [&](i32 begin, i32 end) {
            for (i32 i = begin; i < end; ++i) hits[static_cast<size_t>(i)].fetch_add(1);
        });
        bool ok = true;
        for (const auto& h : hits) ok = ok && h.load() == 1;
        CHECK(ok);
    }
    // Back-to-back jobs and nested calls must not deadlock or lose work.
    std::atomic<i64> sum{0};
    for (int round = 0; round < 200; ++round) {
        jobs.parallel_for(100, 5, [&](i32 begin, i32 end) {
            jobs.parallel_for(end - begin, 1, [&](i32 b, i32 e) { sum.fetch_add(e - b); });
        });
    }
    CHECK(sum.load() == 200 * 100);
}

TEST(fixed_step_accumulates) {
    FixedStep step(60.0);
    int total = 0;
    for (int i = 0; i < 600; ++i) total += step.advance(1.0 / 144.0);
    CHECK(total >= 249 && total <= 250);
    CHECK(step.alpha() >= 0.0 && step.alpha() < 1.0);
    CHECK(step.advance(10.0) <= 16);  // a stall cannot trigger a step avalanche
}

TEST(camera_round_trips_and_zooms_about_a_point) {
    Camera2D camera;
    camera.center = {100.0f, 60.0f};
    camera.zoom = 24.0f;
    camera.viewport = {1280.0f, 720.0f};
    const Vec2 world{113.25f, 48.5f};
    const Vec2 back = camera.screen_to_world(camera.world_to_screen(world));
    CHECK(std::fabs(back.x - world.x) < 1.0e-3f && std::fabs(back.y - world.y) < 1.0e-3f);

    const Vec2 anchor{900.0f, 200.0f};
    const Vec2 before = camera.screen_to_world(anchor);
    camera.zoom_at(anchor, 1.7f, 1.0f, 128.0f);
    const Vec2 after = camera.screen_to_world(anchor);
    CHECK(std::fabs(before.x - after.x) < 1.0e-2f && std::fabs(before.y - after.y) < 1.0e-2f);
}

// --- map generation -----------------------------------------------------------

bool same_map(const TileMap& a, const TileMap& b) {
    return a.level == b.level && a.terrain == b.terrain && a.resource == b.resource && a.flags == b.flags && a.elevation == b.elevation &&
           a.moisture == b.moisture && a.shore_dist == b.shore_dist;
}

TEST(mapgen_is_deterministic_across_thread_counts) {
    MapGenParams params;
    params.seed = 1234;
    params.size = 256;
    JobSystem one(1), four(4);
    const TileMap a = generate_map(params, one);
    const TileMap b = generate_map(params, four);
    const TileMap c = generate_map(params, four);
    CHECK(same_map(a, b));
    CHECK(same_map(b, c));
    params.seed = 1235;
    CHECK(!same_map(a, generate_map(params, four)));
}

void check_map_invariants(const TileMap& map, const MapGenParams& params) {
    const MapStats& s = map.stats;
    const f64 tiles = static_cast<f64>(map.tile_count());

    // The colony site is open, level ground with room to build.
    CHECK(map.walkable(map.start.x, map.start.y));
    CHECK(map.buildable(map.start.x, map.start.y));
    CHECK(s.buildable_near_start >= 300 * map.resolution * map.resolution);

    // The horde must be able to reach the colony, and the colony most of the map.
    CHECK(s.reachable_border_tiles > 0);
    CHECK(s.reachable_tiles >= s.walkable_tiles * 0.9);
    CHECK(s.walkable_tiles >= tiles * 0.35);

    // Requested coverage is met approximately (rivers, beaches and clean-up shift it a little).
    const f64 water = s.terrain_tiles[static_cast<int>(Terrain::Water)] / tiles;
    const f64 mountain = s.terrain_tiles[static_cast<int>(Terrain::Mountain)] / tiles;
    CHECK(water > params.water * 0.6 && water < params.water + 0.08);
    CHECK(mountain > params.mountains * 0.6 && mountain < params.mountains + 0.03);
    CHECK(s.terrain_tiles[static_cast<int>(Terrain::Forest)] > tiles * 0.08);

    // Early-game resources are within reach.
    bool stone_near = false, forest_near = false;
    int resource_tiles = 0;
    for (i32 y = 0; y < map.height; ++y) {
        for (i32 x = 0; x < map.width; ++x) {
            const size_t i = static_cast<size_t>(map.index(x, y));
            const Terrain t = map.terrain[i];
            const u8 f = map.flags[i];
            const f32 dx = static_cast<f32>(x - map.start.x), dy = static_cast<f32>(y - map.start.y);
            const f32 d = std::sqrt(dx * dx + dy * dy) / static_cast<f32>(map.resolution);  // world units
            if (map.resource[i] == Resource::Stone && d <= 36.0f) stone_near = true;
            if (t == Terrain::Forest && d <= 40.0f) forest_near = true;

            // Flags agree with what the tile is.
            const bool blocked = t == Terrain::Water || t == Terrain::Forest || t == Terrain::Mountain || t == Terrain::Rocks || map.resource[i] != Resource::None;
            CHECK(((f & TileFlag::Walkable) != 0) == !blocked);
            if (f & TileFlag::Buildable) CHECK((f & TileFlag::Walkable) != 0 && t != Terrain::Shallows && !(f & TileFlag::Ramp));
            if (f & TileFlag::Ramp) CHECK((f & TileFlag::Walkable) != 0);
            CHECK(map.level[i] < kHeightLevels);
            // No cliff runs through the colony site.
            if (d <= 10.0f) CHECK(map.level[i] == map.level[static_cast<size_t>(map.index(map.start.x, map.start.y))]);
            if (f & TileFlag::River) CHECK(t == Terrain::Water || t == Terrain::Shallows);
            CHECK((map.shore_dist[i] > 0) == (t == Terrain::Water));
            CHECK(map.elevation[i] >= 0.0f && map.elevation[i] <= 1.0f);
            CHECK(map.moisture[i] >= 0.0f && map.moisture[i] <= 1.0f);
            if (map.resource[i] != Resource::None) ++resource_tiles;
        }
    }
    CHECK(stone_near);
    CHECK(forest_near);
    CHECK(resource_tiles > 0);

    // Height: there is high ground, cliffs bound it and ramps lead up to it.
    CHECK(s.level_tiles[1] > 0);
    CHECK(s.cliff_edges > 0);
    CHECK(s.ramps > 0);

    // Walking tile to tile with can_step from the start reaches exactly the
    // tiles the generator reported as reachable.
    std::vector<u8> seen(static_cast<size_t>(map.tile_count()), 0);
    std::vector<i32> frontier{map.index(map.start.x, map.start.y)};
    seen[static_cast<size_t>(frontier[0])] = 1;
    i32 reached = 0;
    while (!frontier.empty()) {
        const i32 i = frontier.back();
        frontier.pop_back();
        ++reached;
        const i32 x = i % map.width, y = i / map.width;
        const i32 next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
        for (const auto& n : next) {
            if (!map.in_bounds(n[0], n[1])) continue;
            const i32 j = map.index(n[0], n[1]);
            if (seen[static_cast<size_t>(j)] || !map.can_step(i, j)) continue;
            seen[static_cast<size_t>(j)] = 1;
            frontier.push_back(j);
        }
    }
    CHECK(reached == s.reachable_tiles);
}

TEST(mapgen_invariants_hold_for_many_seeds) {
    JobSystem jobs;
    for (u64 seed = 1; seed <= 24; ++seed) {
        MapGenParams params;
        params.seed = seed * 7919;
        // The same spread of world sizes (128 to 256 units), on coarse and fine grids.
        params.resolution = seed % 2 == 0 ? 1 : 2;
        params.size = (seed % 3 == 0 ? 128 : (seed % 3 == 1 ? 192 : 256)) * params.resolution;
        const int before = g_failures;
        check_map_invariants(generate_map(params, jobs), params);
        if (g_failures != before) {
            std::printf("    (seed %llu, size %d, resolution %d)\n", static_cast<unsigned long long>(params.seed), params.size, params.resolution);
        }
    }
}

TEST(mapgen_handles_extreme_parameters) {
    JobSystem jobs;
    MapGenParams dry;
    dry.seed = 5;
    dry.size = 96;
    dry.resolution = 1;
    dry.water = 0.0f;
    dry.rivers = 0.0f;
    dry.forest = 0.0f;
    const TileMap desert = generate_map(dry, jobs);
    CHECK(desert.stats.terrain_tiles[static_cast<int>(Terrain::Water)] == 0);
    CHECK(desert.stats.terrain_tiles[static_cast<int>(Terrain::Forest)] == 0);
    CHECK(desert.walkable(desert.start.x, desert.start.y));

    MapGenParams flat;
    flat.seed = 9;
    flat.size = 128;
    flat.resolution = 1;
    flat.uplands = 0.0f;
    const TileMap plain = generate_map(flat, jobs);
    CHECK(plain.stats.cliff_edges == 0 && plain.stats.ramps == 0);
    CHECK(plain.stats.level_tiles[1] == 0 && plain.stats.level_tiles[2] == 0);

    MapGenParams harsh;
    harsh.seed = 6;
    harsh.size = 160;
    harsh.resolution = 1;
    harsh.water = 0.30f;
    harsh.mountains = 0.30f;
    harsh.forest = 0.6f;
    harsh.rivers = 3.0f;
    const TileMap rugged = generate_map(harsh, jobs);
    CHECK(rugged.walkable(rugged.start.x, rugged.start.y));
    CHECK(rugged.stats.reachable_border_tiles > 0);
    CHECK(rugged.stats.buildable_near_start >= 300);
}

// Resolution divides the same world into a finer grid: the geography must stay
// put while every feature gains tiles.
TEST(mapgen_resolution_keeps_the_geography) {
    JobSystem jobs;
    MapGenParams coarse, fine;
    coarse.seed = fine.seed = 4242;
    coarse.resolution = 1;
    coarse.size = 192;
    fine.resolution = 4;
    fine.size = 768;
    const TileMap a = generate_map(coarse, jobs), b = generate_map(fine, jobs);
    check_map_invariants(b, fine);

    // Lakes and mountains come straight from the shared elevation field, so
    // nearly every coarse tile agrees with the fine tile at its centre.
    int compared = 0, agree = 0;
    for (i32 y = 0; y < a.height; ++y) {
        for (i32 x = 0; x < a.width; ++x) {
            const Terrain ta = a.terrain[static_cast<size_t>(a.index(x, y))];
            const Terrain tb = b.terrain[static_cast<size_t>(b.index(x * 4 + 2, y * 4 + 2))];
            const auto broad = [](Terrain t) { return t == Terrain::Mountain ? 2 : (t == Terrain::Water ? 1 : 0); };
            ++compared;
            agree += broad(ta) == broad(tb);
        }
    }
    CHECK(agree >= compared * 0.93);

    // Sixteen times the tiles, so about sixteen times the room.
    const f64 ratio = static_cast<f64>(b.stats.walkable_tiles) / a.stats.walkable_tiles;
    CHECK(ratio > 13.0 && ratio < 19.0);
    CHECK(b.stats.buildable_near_start > 10 * a.stats.buildable_near_start);
}

// --- art ----------------------------------------------------------------------

bool same_pixels(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return false;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const Color& p = a.pixels[i];
        const Color& q = b.pixels[i];
        if (p.r != q.r || p.g != q.g || p.b != q.b || p.a != q.a) return false;
    }
    return true;
}

TEST(png_round_trips) {
    Image image(37, 21);
    for (i32 y = 0; y < image.height; ++y) {
        for (i32 x = 0; x < image.width; ++x) {
            const u32 h = hash2(x, y, 11u);
            image.at(x, y) = {static_cast<u8>(h), static_cast<u8>(h >> 8), static_cast<u8>(h >> 16), static_cast<u8>(h >> 24)};
        }
    }
    const std::string path = (std::filesystem::temp_directory_path() / "zl_test_round_trip.png").string();
    CHECK(write_png(path.c_str(), image));
    Image back;
    CHECK(read_png(path.c_str(), back));
    CHECK(same_pixels(image, back));
    std::filesystem::remove(path);

    // A missing file is reported and leaves the image alone.
    CHECK(!read_png(path.c_str(), back));
    CHECK(same_pixels(image, back));
}

void check_art_is_well_formed(const TerrainArt& art) {
    CHECK(art.ground.size() == static_cast<size_t>(kTerrainCount) + 1);
    for (const Image& layer : art.ground) {
        CHECK(layer.width == kArtTilePixels * kGroundRepeatTiles && layer.height == layer.width);
    }

    const SpriteSet TerrainArt::* sets[] = {&TerrainArt::broadleaf, &TerrainArt::conifer, &TerrainArt::bush,   &TerrainArt::stone,
                                            &TerrainArt::iron,      &TerrainArt::gold,    &TerrainArt::tuft,   &TerrainArt::flower,
                                            &TerrainArt::pebble,    &TerrainArt::reed,    &TerrainArt::boulder};
    const int counts[] = {6, 4, 3, 3, 3, 3, 4, 4, 3, 3, 4};
    int total = 0;
    for (size_t i = 0; i < std::size(sets); ++i) {
        const SpriteSet& set = art.*sets[i];
        CHECK(set.count == counts[i]);
        CHECK(set.first >= 0 && set.first + set.count <= static_cast<i32>(art.defs.size()));
        total += set.count;
    }
    CHECK(static_cast<size_t>(total) == art.defs.size());

    for (const SpriteDef& def : art.defs) {
        CHECK(def.u1 > def.u0 && def.v1 > def.v0);
        CHECK(def.anchor_x > 0.0f && def.anchor_x < def.width && def.anchor_y > 0.0f && def.anchor_y < def.height);
        // Every sprite has something in it.
        const f32 scale = static_cast<f32>(art.sprites.width) / 65535.0f;
        bool visible = false;
        for (i32 y = static_cast<i32>(def.v0 * scale); y < static_cast<i32>(def.v1 * scale) && !visible; ++y) {
            for (i32 x = static_cast<i32>(def.u0 * scale); x < static_cast<i32>(def.u1 * scale) && !visible; ++x) visible = art.sprites.at(x, y).a > 128;
        }
        CHECK(visible);
    }

    // The atlas is drawn with premultiplied alpha: no colour can exceed its coverage.
    bool premultiplied = true;
    for (const Color& c : art.sprites.pixels) premultiplied = premultiplied && c.r <= c.a && c.g <= c.a && c.b <= c.a;
    CHECK(premultiplied);
}

TEST(painted_art_loads_from_files) {
    JobSystem jobs(4);
    const TerrainArt painted = load_terrain_art(jobs, ZL_ASSET_DIR);
    CHECK(painted.painted_ground);
    CHECK(painted.painted_sprites);
    check_art_is_well_formed(painted);

    const TerrainArt drawn = build_terrain_art(jobs);
    CHECK(!drawn.painted_ground && !drawn.painted_sprites);
    check_art_is_well_formed(drawn);

    // Files replace the placeholders, except for the layers that are shader data.
    const auto layer = [](const TerrainArt& art, int index) -> const Image& { return art.ground[static_cast<size_t>(index)]; };
    CHECK(!same_pixels(layer(painted, static_cast<int>(Terrain::Grass)), layer(drawn, static_cast<int>(Terrain::Grass))));
    CHECK(!same_pixels(painted.sprites, drawn.sprites));
    CHECK(same_pixels(layer(painted, static_cast<int>(Terrain::Water)), layer(drawn, static_cast<int>(Terrain::Water))));
    CHECK(same_pixels(layer(painted, kNoiseLayer), layer(drawn, kNoiseLayer)));
    for (const Color& c : layer(painted, static_cast<int>(Terrain::Grass)).pixels) CHECK(c.a == 255);
}

TEST(art_falls_back_to_placeholders_without_files) {
    JobSystem jobs(4);
    const std::string missing = (std::filesystem::temp_directory_path() / "zl_test_no_such_assets").string();
    const TerrainArt fallback = load_terrain_art(jobs, missing.c_str());
    CHECK(!fallback.painted_ground && !fallback.painted_sprites);
    check_art_is_well_formed(fallback);

    // The placeholders do not depend on how many threads drew them.
    JobSystem single(1);
    const TerrainArt drawn = build_terrain_art(single);
    CHECK(same_pixels(fallback.sprites, drawn.sprites));
    for (size_t i = 0; i < drawn.ground.size(); ++i) CHECK(same_pixels(fallback.ground[i], drawn.ground[i]));
}

}  // namespace

int main() {
    int failed_tests = 0;
    for (const TestCase& test : registry()) {
        const int before = g_failures;
        const Stopwatch timer;
        test.fn();
        const bool ok = g_failures == before;
        failed_tests += ok ? 0 : 1;
        std::printf("[%s] %s (%.0f ms)\n", ok ? " ok " : "FAIL", test.name, timer.millis());
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
