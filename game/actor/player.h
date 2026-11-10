// The character the player walks about the map: movement, collision with the
// terrain, and the state that drives the human rig. Headless and stepped at a
// fixed rate, so a seed and a sequence of inputs always give the same walk.
#pragma once

#include "actor/human.h"
#include "core/base.h"
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

// Whether a body of the given radius (in tiles) can stand at `position`: every
// tile it touches must be walkable and reachable from the tile under its
// centre without crossing a cliff (see TileMap::can_step).
bool can_stand(const TileMap& map, Vec2 position, f32 radius);

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

}  // namespace zl
