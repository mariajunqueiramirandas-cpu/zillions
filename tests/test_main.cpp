// Headless test suite. A deliberately tiny harness: TEST registers a function,
// CHECK records a failure and carries on, main runs everything.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "actor/player.h"
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

// --- character ---------------------------------------------------------------

bool near(Vec3 a, Vec3 b, f32 epsilon = 2.0e-5f) { return length(a - b) < epsilon; }
bool finite(Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

std::vector<Vec3> joints(const HumanSkeleton& s) {
    std::vector<Vec3> result{s.pelvis, s.waist, s.chest, s.neck, s.head};
    for (int i = 0; i < 2; ++i) {
        for (Vec3 p : {s.hip[i], s.knee[i], s.ankle[i], s.heel[i], s.ball[i], s.toe[i],
                       s.shoulder[i], s.elbow[i], s.wrist[i], s.fingertip[i], s.thumb[i]}) result.push_back(p);
    }
    return result;
}

bool same_figure(const Figure& a, const Figure& b) {
    if (a.parts.size() != b.parts.size() || a.waterline != b.waterline) return false;
    for (size_t i = 0; i < a.parts.size(); ++i) {
        const auto &x = a.parts[i], &y = b.parts[i];
        if (!near(x.a, y.a) || !near(x.b, y.b) || x.ra != y.ra || x.rb != y.rb ||
            x.color.r != y.color.r || x.color.g != y.color.g || x.color.b != y.color.b ||
            x.color.a != y.color.a || x.material != y.material || x.flags != y.flags ||
            x.group != y.group || x.roundness != y.roundness) return false;
    }
    return true;
}

struct HandClearance { f32 distance = 1.0e9f; int group = 0; };

HandClearance hand_gear_clearance(const HumanLook& look, const HumanMotion& motion, const HumanSkeleton& skeleton) {
    Figure figure;
    build_human(look, motion, figure);
    const f32 scale = kTilesPerMetre * look.height;
    const auto in_figure = [&](Vec3 p) {
        const Vec2 front{std::cos(motion.heading), std::sin(motion.heading)}, side{-front.y, front.x};
        return Vec3{side.x * p.x + front.x * p.y, side.y * p.x + front.y * p.y, p.z} * scale;
    };
    HandClearance closest;
    for (int i = 0; i < 2; ++i) {
        const Vec3 points[] = {skeleton.wrist[i], skeleton.fingertip[i], skeleton.thumb[i]};
        const f32 radii[] = {0.032f, 0.024f, 0.010f};
        for (int j = 0; j < 3; ++j) {
            for (const FigurePart& part : figure.parts) {
                if ((part.material != FigureMaterial::Leather && part.material != FigureMaterial::Metal) || (part.flags & FigurePartFlag::FlatCaps)) continue;
                const f32 distance = round_cone_distance(in_figure(points[j]), part.a, part.b, part.ra, part.rb) / scale - radii[j];
                if (distance < closest.distance) closest = {distance, part.group};
            }
        }
    }
    return closest;
}

TEST(human_rig_keeps_bones_soles_and_parts_valid) {
    bool bones = true, soles = true, values = true, capacity = true, bounds = true;
    for (u64 seed = 0; seed < 24; ++seed) {
        HumanLook look = seed == 0 ? hero_look() : random_look(seed);
        look.beard = true; // Exercise the largest combinations too.
        for (f32 speed : {0.0f, 0.2f, 0.6f, kWalkSpeed, 2.4f, kJogSpeed, kSprintSpeed, 6.7f}) {
            for (int phase = 0; phase <= 64; ++phase) {
                HumanMotion m;
                m.speed = speed;
                m.phase = static_cast<f32>(phase) / 64.0f;
                m.heading = static_cast<f32>(seed) * 0.7f;
                m.time = 1.4f;
                m.turn = phase % 2 ? -2.0f : 2.0f;
                m.lag = {2.0f, -3.0f};
                const HumanSkeleton s = pose_human(look, m);
                for (Vec3 p : joints(s)) values &= finite(p);
                for (int i = 0; i < 2; ++i) {
                    bones &= std::fabs(length(s.knee[i] - s.hip[i]) - kThighLength) < 2.0e-5f;
                    bones &= std::fabs(length(s.ankle[i] - s.knee[i]) - kShinLength) < 2.0e-5f;
                    bones &= std::fabs(length(s.elbow[i] - s.shoulder[i]) - kUpperArmLength) < 2.0e-5f;
                    bones &= std::fabs(length(s.wrist[i] - s.elbow[i]) - kForearmLength) < 2.0e-5f;
                    soles &= std::min({s.heel[i].z, s.ball[i].z, s.toe[i].z}) >= -2.0e-5f;
                }
                Figure f;
                build_human(look, m, f);
                capacity &= !f.parts.empty() && f.parts.size() < static_cast<size_t>(kMaxFigureParts);
                Vec2 lo, hi;
                bounds &= figure_screen_bounds(f, lo, hi);
                for (const FigurePart& p : f.parts) {
                    values &= finite(p.a) && finite(p.b) && std::isfinite(p.ra) && std::isfinite(p.rb) &&
                              p.ra > 0.0f && p.rb > 0.0f && std::isfinite(p.roundness);
                    for (Vec3 end : {p.a, p.b}) {
                        const Vec2 screen = figure_to_screen(end);
                        bounds &= screen.x >= lo.x && screen.x <= hi.x && screen.y >= lo.y && screen.y <= hi.y;
                    }
                }
            }
        }
    }
    CHECK(bones); CHECK(soles); CHECK(values); CHECK(capacity); CHECK(bounds);
}

TEST(human_pose_wraps_and_planted_feet_hold_the_ground) {
    const HumanLook look = hero_look();
    for (f32 speed : {0.0f, 0.6f, kWalkSpeed, 2.4f, kJogSpeed, kSprintSpeed}) {
        HumanMotion a;
        a.speed = speed;
        a.time = 1.0f;
        HumanMotion b = a;
        b.phase = 1.0f;
        const auto ja = joints(pose_human(look, a)), jb = joints(pose_human(look, b));
        for (size_t i = 0; i < ja.size(); ++i) CHECK(near(ja[i], jb[i]));
        const HumanGait gait = human_gait(speed);
        if (speed == 0.0f) continue;
        // Between heel roll and toe-off the whole sole stays flat.
        for (int leg = 0; leg < 2; ++leg) {
            a.phase = gait.duty * 0.23f + static_cast<f32>(leg) * 0.5f;
            b.phase = gait.duty * 0.32f + static_cast<f32>(leg) * 0.5f;
            const auto sa = pose_human(look, a), sb = pose_human(look, b);
            CHECK(sa.planted[leg] && sb.planted[leg]);
            const Vec3 travel{0.0f, (b.phase - a.phase) * gait.cycle, 0.0f};
            CHECK(near(sa.heel[leg], sb.heel[leg] + travel));
            CHECK(near(sa.ball[leg], sb.ball[leg] + travel));
            CHECK(near(sa.toe[leg], sb.toe[leg] + travel));
        }
    }
}

TEST(human_arms_keep_natural_bends_and_clear_the_gear) {
    bool bends = true, reach = true, continuous = true, clear = true, torso_clear = true;
    f32 minimum_clearance = 1.0e9f;
    f32 worst_speed = 0, worst_build = 0, worst_phase = 0;
    int worst_group = 0;
    for (f32 build : {0.90f, 1.0f, 1.12f}) {
        for (bool satchel : {false, true}) {
            HumanLook look = hero_look();
            look.build = build;
            look.height = build < 1.0f ? 0.93f : 1.06f;
            look.satchel = satchel;
            for (f32 speed : {0.0f, 0.6f, kWalkSpeed, 2.4f, kJogSpeed, kSprintSpeed, 6.7f}) {
                HumanSkeleton previous;
                f32 counter_swing = 0;
                for (int phase = 0; phase <= 128; ++phase) {
                    HumanMotion motion;
                    motion.speed = speed;
                    motion.phase = static_cast<f32>(phase) / 128.0f;
                    motion.time = 0.9f;
                    motion.turn = 1.5f;
                    motion.lag = {2, -3};
                    const HumanSkeleton s = pose_human(look, motion);
                    const Vec3 right = normalize(s.shoulder[1] - s.shoulder[0]);
                    const Vec3 up = normalize(s.neck - s.chest);
                    const Vec3 forward = normalize(cross(up, right));
                    for (int i = 0; i < 2; ++i) {
                        const Vec3 upper = normalize(s.elbow[i] - s.shoulder[i]), fore = normalize(s.wrist[i] - s.elbow[i]);
                        const f32 angle = std::acos(clamp(dot(upper, fore), -1.0f, 1.0f)) * 180.0f / kPi;
                        bends &= angle >= 5.0f && angle <= 110.0f;
                        if (speed == kWalkSpeed) {
                            bends &= angle <= 30.0f;
                            reach &= dot(s.fingertip[i] - s.shoulder[i], forward) < 0.30f;
                        }
                        if (speed >= kJogSpeed) bends &= angle >= 70.0f;
                        if (phase) {
                            continuous &= length(s.thumb[i] - previous.thumb[i]) < 0.035f;
                            continuous &= length(s.wrist[i] - previous.wrist[i]) < 0.035f;
                        }
                    }
                    counter_swing += (s.wrist[0].y - s.wrist[1].y) * (s.ankle[0].y - s.ankle[1].y);
                    previous = s;
                    const HandClearance gear = hand_gear_clearance(look, motion, s);
                    if (gear.distance < minimum_clearance) {
                        minimum_clearance = gear.distance;
                        worst_speed = speed; worst_build = build; worst_phase = motion.phase; worst_group = gear.group;
                    }
                    clear &= gear.distance > -0.002f;
                    for (int i = 0; i < 2; ++i) {
                        const Vec3 points[] = {s.wrist[i], s.fingertip[i], s.thumb[i]};
                        const f32 radii[] = {0.032f, 0.024f, 0.010f};
                        for (int j = 0; j < 3; ++j) {
                            // An independent torso envelope, derived from the
                            // spine and shoulder axes rather than skin parts.
                            const Vec3 spine = s.chest - s.pelvis;
                            const f32 along = dot(points[j] - s.pelvis, spine) / dot(spine, spine);
                            if (along >= 0 && along <= 1) {
                                const Vec3 offset = points[j] - lerp(s.pelvis, s.chest, along);
                                const f32 x = dot(offset, right) / (0.16f * build + radii[j]);
                                const f32 y = dot(offset, forward) / (0.10f * build + radii[j]);
                                torso_clear &= x * x + y * y >= 1.0f;
                            }
                        }
                    }
                }
                if (speed >= 0.6f) CHECK(counter_swing < 0.0f);
            }
        }
    }
    if (!clear) std::printf("    hand/gear clearance %.6f metres (speed %.2f, build %.2f, phase %.3f, group %d)\n", minimum_clearance, worst_speed, worst_build, worst_phase, worst_group);
    CHECK(bends); CHECK(reach); CHECK(continuous); CHECK(clear); CHECK(torso_clear);
}

TEST(human_looks_are_repeatable_and_varied) {
    Figure a, b, other;
    HumanMotion m;
    for (u64 seed = 1; seed <= 128; ++seed) {
        build_human(random_look(seed), m, a);
        build_human(random_look(seed), m, b);
        build_human(random_look(seed + 1), m, other);
        CHECK(same_figure(a, b));
        CHECK(!same_figure(a, other));
    }
}

TEST(figure_distances_and_empty_bounds) {
    const Vec3 a{0, 0, 0}, b{0, 0, 2};
    CHECK(std::fabs(round_cone_distance({1, 0, 1}, a, b, 0.5f, 0.5f) - 0.5f) < 1.0e-6f);
    CHECK(std::fabs(round_cone_distance({0, 0, 3}, a, b, 0.5f, 0.5f) - 0.5f) < 1.0e-6f);
    CHECK(std::fabs(round_cone_distance({0, 0, 0}, a, a, 0.5f, 0.2f) + 0.5f) < 1.0e-6f);
    Vec2 lo, hi;
    CHECK(!figure_screen_bounds(Figure{}, lo, hi));
}

TileMap actor_map(Terrain terrain = Terrain::Grass) {
    TileMap map;
    map.width = map.height = 32;
    map.start = {4, 4};
    map.terrain.assign(1024, terrain);
    map.flags.assign(1024, TileFlag::Walkable);
    map.level.assign(1024, 0);
    return map;
}

void walk_steps(Player& player, const TileMap& map, Vec2 direction, int count, bool sprint = false) {
    for (int i = 0; i < count; ++i) player.step(map, {direction, sprint, false}, 1.0f / 60.0f);
}

TEST(player_blocks_obstacles_and_cliffs_but_crosses_ramps) {
    for (Terrain barrier : {Terrain::Water, Terrain::Forest, Terrain::Mountain, Terrain::Rocks}) {
        TileMap map = actor_map();
        for (int y = 0; y < map.height; ++y) {
            const size_t i = static_cast<size_t>(map.index(8, y));
            map.terrain[i] = barrier;
            map.flags[i] = 0;
        }
        Player p;
        CHECK(p.spawn(map));
        walk_steps(p, map, {1, 0}, 240, true);
        CHECK(p.position().x <= 8.0f - Player::kRadius + 1.0e-5f);
        CHECK(p.motion().speed < 0.01f);
        const f32 y = p.position().y;
        walk_steps(p, map, {1, 1}, 120);
        CHECK(p.position().y > y + 2.0f); // Slides along the wall.
        CHECK(can_stand(map, p.position(), Player::kRadius));
    }
    TileMap map = actor_map();
    for (int y = 0; y < map.height; ++y)
        for (int x = 8; x < map.width; ++x) map.level[static_cast<size_t>(map.index(x, y))] = 1;
    Player p;
    CHECK(p.spawn(map));
    walk_steps(p, map, {1, 0}, 240, true);
    CHECK(p.position().x < 8.0f);
    map.flags[static_cast<size_t>(map.index(8, 4))] |= TileFlag::Ramp;
    walk_steps(p, map, {1, 0}, 120);
    CHECK(p.position().x > 10.0f);
}

TEST(player_fords_slow_movement_and_replays_match) {
    const TileMap dry = actor_map(), wet = actor_map(Terrain::Shallows);
    Player a, b, w;
    CHECK(a.spawn(dry)); CHECK(b.spawn(dry)); CHECK(w.spawn(wet));
    CHECK(w.motion().wade == kWadeDepth); // Posed screenshots start submerged too.
    for (int i = 0; i < 360; ++i) {
        const PlayerInput input{{i < 180 ? 1.0f : 0.0f, i < 180 ? 0.0f : 1.0f}, true, false};
        a.step(dry, input, 1.0f / 60.0f);
        b.step(dry, input, 1.0f / 60.0f);
        w.step(wet, input, 1.0f / 60.0f);
        CHECK(a.position().x == b.position().x && a.position().y == b.position().y);
        CHECK(a.motion().phase == b.motion().phase && a.motion().heading == b.motion().heading);
        CHECK(a.motion().body_activity == b.motion().body_activity && a.motion().body_run == b.motion().body_run);
    }
    CHECK(length(w.position() - Vec2{4.5f, 4.5f}) < length(a.position() - Vec2{4.5f, 4.5f}) * 0.5f);
    CHECK(std::fabs(w.motion().wade - kWadeDepth) < 1.0e-4f);
    CHECK(std::fabs(length(w.velocity()) / length(a.velocity()) - kJogSpeed * kWadeSlowdown / kSprintSpeed) < 0.01f);
}

TEST(player_arm_transitions_settle_without_snaps) {
    const TileMap map = actor_map();
    Player player;
    CHECK(player.spawn(map));
    HumanSkeleton previous = pose_human(player.look, player.motion());
    HumanMotion old = player.motion();
    f32 max_elbow_change = 0;
    f32 minimum_gear_clearance = 1.0e9f;
    for (int frame = 0; frame < 720; ++frame) {
        PlayerInput input;
        if (frame < 180) input = {{1, 0}, false, true};
        else if (frame < 360) input = {{0, 1}, false, false};
        else if (frame < 540) input = {{-1, 0}, true, false};
        player.step(map, input, 1.0f / 60.0f);
        const HumanMotion motion = player.motion();
        const HumanSkeleton s = pose_human(player.look, motion);
        minimum_gear_clearance = std::min(minimum_gear_clearance, hand_gear_clearance(player.look, motion, s).distance);
        CHECK(motion.body_activity >= 0 && motion.body_activity <= 1 && motion.body_run >= 0 && motion.body_run <= 1);
        CHECK(std::fabs(motion.heading - old.heading) <= 3.0f * kPi / 60.0f + 1.0e-5f);
        for (int i = 0; i < 2; ++i) {
            const auto elbow_angle = [i](const HumanSkeleton& pose) {
                return std::acos(clamp(dot(normalize(pose.elbow[i] - pose.shoulder[i]), normalize(pose.wrist[i] - pose.elbow[i])), -1.0f, 1.0f));
            };
            max_elbow_change = std::max(max_elbow_change, std::fabs(elbow_angle(s) - elbow_angle(previous)));
            if (std::fabs(elbow_angle(s) - elbow_angle(previous)) > 12.0f * kPi / 180.0f)
                std::printf("    arm jump frame %d speed %.2f activity %.3f run %.3f, elbow %.1f -> %.1f\n", frame, motion.speed, motion.body_activity, motion.body_run, elbow_angle(previous) * 180.0f / kPi, elbow_angle(s) * 180.0f / kPi);
            CHECK(length(s.wrist[i] - previous.wrist[i]) < 0.10f);
        }
        previous = s;
        old = motion;
    }
    CHECK(max_elbow_change < 12.0f * kPi / 180.0f);
    if (minimum_gear_clearance < -0.002f) std::printf("    moving hand/gear clearance %.6f metres\n", minimum_gear_clearance);
    CHECK(minimum_gear_clearance > -0.002f);
    CHECK(player.motion().body_activity < 0.001f && player.motion().body_run < 0.001f);
    CHECK(player.motion().speed == 0.0f);
}

TEST(player_edges_corners_and_interpolation_remain_safe) {
    TileMap map = actor_map();
    map.flags[static_cast<size_t>(map.index(5, 4))] = 0;
    CHECK(!can_stand(map, {4.9f, 4.5f}, Player::kRadius));
    CHECK(!can_stand(map, {0.1f, 0.5f}, Player::kRadius));
    CHECK(!can_stand(map, {31.9f, 31.5f}, Player::kRadius));
    // The circle overlaps a diagonally adjacent blocked tile.
    CHECK(!can_stand(map, {4.9f, 5.1f}, Player::kRadius));
    Player p;
    CHECK(p.spawn(map, {5.5f, 4.5f})); // Relocates off the obstacle.
    CHECK(can_stand(map, p.position(), Player::kRadius));
    const Vec2 before = p.position();
    p.step(map, {{1, 0}, false, false}, 0.0f);
    CHECK(p.position().x == before.x && p.position().y == before.y);
    CHECK(finite({p.velocity().x, p.velocity().y, p.motion().speed}));
    CHECK(p.spawn(map, {3.5f, 3.5f})); // An unobstructed lane for phase wrapping.
    bool wrapped = false;
    for (int i = 0; i < 240; ++i) {
        const Vec2 previous = p.position();
        const HumanMotion old = p.motion();
        p.step(map, {{0, 1}, false, false}, 1.0f / 60.0f);
        const Vec2 middle = p.position_at(0.5f);
        CHECK(length(middle - (previous + p.position()) * 0.5f) < 1.0e-5f);
        const HumanMotion m = p.motion_at(0.5f);
        CHECK(m.phase >= 0.0f && m.phase < 1.0f && std::isfinite(m.heading));
        if (p.motion().phase < old.phase) {
            wrapped = true;
            CHECK(std::min(m.phase, 1.0f - m.phase) < 0.04f);
        }
    }
    CHECK(wrapped);
    map.flags.assign(map.flags.size(), 0);
    CHECK(!p.spawn(map));
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

struct ArtTestDirectory {
    std::filesystem::path path;

    ArtTestDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int i = 0; i < 100; ++i) {
            path = std::filesystem::temp_directory_path() / ("zl_art_" + std::to_string(stamp) + "_" + std::to_string(i));
            if (std::filesystem::create_directory(path)) return;
        }
        ZL_CHECK(false, "cannot create an art test directory");
    }
    ~ArtTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    void copy_assets() const {
        for (const char* group : {"ground", "sprites"}) {
            std::filesystem::copy(std::filesystem::path(ZL_ASSET_DIR) / group, path / group, std::filesystem::copy_options::recursive);
        }
    }
};

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
    const ArtTestDirectory directory;
    const std::string path = (directory.path / "round_trip.png").string();
    CHECK(write_png(path.c_str(), image));
    Image back;
    CHECK(read_png(path.c_str(), back));
    CHECK(same_pixels(image, back));
    std::filesystem::remove(path);

    // A missing file is reported and leaves the image alone.
    CHECK(!read_png(path.c_str(), back));
    CHECK(same_pixels(image, back));

    // Corrupt files, including a truncated PNG, must not destroy the caller's image.
    for (const std::string& contents : {std::string("not a PNG"), std::string("\x89PNG\r\n\x1a\n", 8)}) {
        std::ofstream(path, std::ios::binary).write(contents.data(), static_cast<std::streamsize>(contents.size()));
        CHECK(!read_png(path.c_str(), back));
        CHECK(same_pixels(image, back));
    }
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

    // Check actual decoded texels, not just that files differ from placeholders.
    const char* names[] = {nullptr, "shallows", "sand", "grass", "dirt", "forest", "mountain", "rocks"};
    for (int t = 1; t < kTerrainCount; ++t) {
        Image source;
        const std::string path = std::string(ZL_ASSET_DIR) + "/ground/" + names[t] + ".png";
        CHECK(read_png(path.c_str(), source));
        CHECK(same_pixels(layer(painted, t), source));
    }

    const char* families[] = {"broadleaf", "conifer", "bush", "stone", "iron", "gold", "tuft", "flower", "pebble", "reed", "boulder"};
    const SpriteSet TerrainArt::* sets[] = {&TerrainArt::broadleaf, &TerrainArt::conifer, &TerrainArt::bush, &TerrainArt::stone,
                                            &TerrainArt::iron, &TerrainArt::gold, &TerrainArt::tuft, &TerrainArt::flower,
                                            &TerrainArt::pebble, &TerrainArt::reed, &TerrainArt::boulder};
    for (size_t f = 0; f < std::size(families); ++f) {
        const SpriteSet& set = painted.*sets[f];
        for (int v = 0; v < set.count; ++v) {
            const SpriteDef& def = painted.defs[static_cast<size_t>(set.first + v)];
            const auto coordinate = [&](u16 uv) {
                return static_cast<i32>(std::lround(static_cast<f32>(uv) * static_cast<f32>(painted.sprites.width) / 65535.0f));
            };
            const i32 ox = coordinate(def.u0), oy = coordinate(def.v0);
            CHECK(ox % kSpriteAtlasPadding == 0 && oy % kSpriteAtlasPadding == 0);
            Image source;
            const std::string path = std::string(ZL_ASSET_DIR) + "/sprites/" + families[f] + "_" + std::to_string(v) + ".png";
            CHECK(read_png(path.c_str(), source));
            CHECK(coordinate(def.u1) - ox == source.width && coordinate(def.v1) - oy == source.height);
            for (i32 y = 0; y < source.height; ++y) {
                for (i32 x = 0; x < source.width; ++x) {
                    const Color& s = source.at(x, y);
                    const Color& a = painted.sprites.at(ox + x, oy + y);
                    CHECK(a.a == s.a);
                    CHECK(a.r == (s.r * s.a + 127u) / 255u);
                    CHECK(a.g == (s.g * s.a + 127u) / 255u);
                    CHECK(a.b == (s.b * s.a + 127u) / 255u);
                }
            }
            // A full transparent mip texel separates every sprite at the
            // coarsest permitted level. No neighbour can enter its filter.
            bool isolated = true;
            for (i32 y = -kSpriteAtlasPadding; y < source.height + kSpriteAtlasPadding; ++y) {
                for (i32 x = -kSpriteAtlasPadding; x < source.width + kSpriteAtlasPadding; ++x) {
                    if (x >= 0 && x < source.width && y >= 0 && y < source.height) continue;
                    isolated = isolated && painted.sprites.at(ox + x, oy + y).a == 0;
                }
            }
            CHECK(isolated);
        }
    }
}

TEST(art_falls_back_to_placeholders_without_files) {
    JobSystem jobs(4);
    const ArtTestDirectory directory;
    const std::string missing = (directory.path / "missing").string();
    const TerrainArt fallback = load_terrain_art(jobs, missing.c_str());
    CHECK(!fallback.painted_ground && !fallback.painted_sprites);
    check_art_is_well_formed(fallback);

    // The placeholders do not depend on how many threads drew them.
    JobSystem single(1);
    const TerrainArt drawn = build_terrain_art(single);
    CHECK(same_pixels(fallback.sprites, drawn.sprites));
    for (size_t i = 0; i < drawn.ground.size(); ++i) CHECK(same_pixels(fallback.ground[i], drawn.ground[i]));
}

TEST(incomplete_art_falls_back_by_group_without_partial_replacement) {
    const ArtTestDirectory directory;
    directory.copy_assets();
    const std::string dir = directory.path.string();
    JobSystem jobs(4);
    const TerrainArt drawn = build_terrain_art(jobs);
    const TerrainArt painted = load_terrain_art(jobs, ZL_ASSET_DIR);

    // Fail on the LAST member of each group: earlier files must not leak into
    // the fallback. The other complete group must still load from disk.
    for (const bool ground : {false, true}) {
        const char* relative = ground ? "ground/rocks.png" : "sprites/boulder_3.png";
        const auto file = directory.path / relative;
        for (int failure = 0; failure < 3; ++failure) {
            std::filesystem::remove(file);
            if (failure == 1) CHECK(write_png(file.string().c_str(), Image(3, 5, Color{255, 0, 0, 255})));
            if (failure == 2) std::ofstream(file, std::ios::binary) << "corrupt PNG";
            const TerrainArt art = load_terrain_art(jobs, dir.c_str());
            CHECK(art.painted_ground == !ground && art.painted_sprites == ground);
            const TerrainArt& expected_ground = ground ? drawn : painted;
            for (size_t i = 0; i < art.ground.size(); ++i) CHECK(same_pixels(art.ground[i], expected_ground.ground[i]));
            CHECK(same_pixels(art.sprites, ground ? painted.sprites : drawn.sprites));
            // Both sources keep identical atlas slots for live reload.
            CHECK(art.defs.size() == painted.defs.size());
            for (size_t i = 0; i < art.defs.size(); ++i) {
                const SpriteDef& a = art.defs[i];
                const SpriteDef& b = painted.defs[i];
                CHECK(a.u0 == b.u0 && a.u1 == b.u1 && a.v0 == b.v0 && a.v1 == b.v1);
                CHECK(a.width == b.width && a.height == b.height && a.anchor_x == b.anchor_x && a.anchor_y == b.anchor_y);
            }
        }
        std::filesystem::copy_file(std::filesystem::path(ZL_ASSET_DIR) / relative, file, std::filesystem::copy_options::overwrite_existing);
    }
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
