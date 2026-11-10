// The character the player walks about the map: movement, collision with the
// terrain, and the state that drives the human rig. Headless and stepped at a
// fixed rate, so a seed and a sequence of inputs always give the same walk.
#pragma once

#include "actor/human.h"
#include "core/base.h"
#include "core/rng.h"
#include "math/vec.h"
#include "world/tilemap.h"

namespace zl {

struct PlayerInput {
    Vec2 move{0.0f, 0.0f};  // where to go, in map directions (+x east, +y south); any length
    bool sprint = false;
    bool walk = false;
};

// Speeds in metres per second at the body's own scale.
inline constexpr f32 kWalkSpeed = 1.5f;
inline constexpr f32 kJogSpeed = 3.0f;
inline constexpr f32 kSprintSpeed = 5.2f;
// Fords are crossed slowly.
inline constexpr f32 kWadeSlowdown = 0.55f;
inline constexpr f32 kWadeDepth = 0.24f;  // metres of water in a ford

// Grounded cylindrical body, in tiles. Terrain edges and cliffs remain solid;
// generated scenery uses its actual trunk/rock footprint and vertical extent.
bool can_stand(const TileMap& map, Vec2 position, f32 radius, f32 height = 1.75f * kTilesPerMetre);

class Player {
public:
    // Radius of the body on the ground, in tiles.
    static constexpr f32 kRadius = 0.26f;

    HumanLook look = hero_look();

    // Stands the player on open ground at or near `where` (by default the
    // colony's start), facing the viewer. Returns false if the map has no
    // ground to stand on.
    bool spawn(const TileMap& map);
    bool spawn(const TileMap& map, Vec2 where);

    // Advances by one fixed step of `dt` seconds.
    void step(const TileMap& map, const PlayerInput& input, f32 dt);

    Vec2 position() const { return position_; }
    Vec2 velocity() const { return velocity_; }  // tiles per second, as actually moved
    f32 body_radius() const { return kRadius * look.height * look.build; }
    f32 body_height() const { return 1.75f * kTilesPerMetre * look.height; }
    const HumanMotion& motion() const { return motion_; }
    // Overrides the pose, for posed screenshots.
    void set_motion(const HumanMotion& motion) { motion_ = previous_motion_ = motion; }

    // The state between the previous step (alpha 0) and the latest (alpha 1),
    // for drawing at any frame rate.
    Vec2 position_at(f32 alpha) const;
    HumanMotion motion_at(f32 alpha) const;

private:
    Vec2 position_{0.0f, 0.0f}, previous_position_{0.0f, 0.0f};
    Vec2 velocity_{0.0f, 0.0f};
    Vec2 drive_{0.0f, 0.0f};  // the velocity being steered towards, smoothed
    HumanMotion motion_, previous_motion_;
};

// Basic local autonomy when the player switches to management mode: patrols
// around an anchor with varied gaits, curved multi-point paths, obstacle
// feelers and idle glances. Headless and deterministic.
class PlayerAutonomy {
public:
    static constexpr f32 kWanderRadius = 7.5f;

    void reset(Vec2 anchor, u64 seed = 1);
    PlayerInput step(const TileMap& map, const Player& player, f32 dt);

    Vec2 anchor() const { return anchor_; }
    Vec2 target() const { return target_; }
    bool moving() const { return moving_; }

private:
    bool pick_waypoint(const TileMap& map, const Player& player, bool continuation);

    Rng rng_{1};
    Vec2 anchor_{0.0f, 0.0f};
    Vec2 target_{0.0f, 0.0f};
    Vec2 glance_dir_{0.0f, 0.0f};
    f32 timer_ = 0.0f;        // time left in idle, or time spent on current leg
    f32 leg_limit_ = 5.0f;    // max duration for the current walk leg
    f32 glance_time_ = -1.0f; // when > 0 during idle, briefly turn towards glance_dir_
    f32 curve_ = 0.0f;        // subtle angular offset for curved paths
    f32 blocked_time_ = 0.0f; // time spent trying to move without covering ground
    u8 pace_ = 0;             // 0: stroll, 1: jog, 2: brief sprint then jog
    u8 chain_left_ = 0;       // waypoints left in a multi-point excursion
    bool moving_ = false;
    bool initialized_ = false;
};

}  // namespace zl
