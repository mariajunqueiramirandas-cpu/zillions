// Static scenery shared by rendering and character collision. Positions and
// footprints are in simulation coordinates, independent of camera and zoom.
#pragma once

#include <vector>

#include "core/image.h"
#include "math/vec.h"
#include "world/scale.h"

namespace zl {
struct TileMap;

enum class SceneryKind : u8 { Broadleaf, Conifer, Bush, Stone, Iron, Gold, Boulder, Tuft, Flower, Pebble, Reed, Branch, Stump, Rubble, Chest, Count };

// One physical catalogue for generated meshes and collision.
// One tile is 1.25 metres: the 1.70 m player is 1.36 tiles tall. Mature trees
// are 8–11 m tall before variation, keeping play-zoom silhouettes readable.
// Crowns never become invisible collision walls.
struct SceneryModelInfo {
    const char* name;
    Vec3 size;  // width, depth, height in tiles, before seeded variation
    f32 trunk_radius;
    f32 half_length;
};
inline constexpr SceneryModelInfo kSceneryModels[] = {
    {"broadleaf", {3.0f, 2.75f, 6.4f}, 0.55f, 0.0f},
    {"conifer", {2.6f, 2.6f, 8.8f}, 0.32f, 0.0f},
    {"bush", {1.5f, 1.3f, 1.0f}, 0, 0},
    {"stone", {2.0f, 1.5f, 1.2f}, 0.70f, 0.25f},
    {"iron", {1.9f, 1.5f, 1.15f}, 0.68f, 0.22f},
    {"gold", {1.8f, 1.4f, 1.1f}, 0.65f, 0.22f},
    {"boulder", {3.0f, 2.4f, 2.5f}, 1.10f, 0.35f},
    {"tuft", {0.45f, 0.4f, 0.45f}, 0, 0},
    {"flower", {0.55f, 0.5f, 0.55f}, 0, 0},
    {"pebble", {0.35f, 0.28f, 0.18f}, 0, 0},
    {"reed", {0.65f, 0.6f, 1.5f}, 0, 0},
    {"branch", {1.4f, 0.55f, 0.22f}, 0, 0},
    {"stump", {1.35f, 1.2f, 0.85f}, 0, 0},
    {"rubble", {1.3f, 1.0f, 0.4f}, 0, 0},
    {"chest", {1.1f, 0.65f, 0.68f}, 0.30f, 0.20f},
};
inline constexpr f32 kSceneryMaxHeight = 12.0f;
static_assert(sizeof(kSceneryModels) / sizeof(kSceneryModels[0]) == static_cast<size_t>(SceneryKind::Count));
inline constexpr const SceneryModelInfo& scenery_model(SceneryKind kind) {
    return kSceneryModels[static_cast<size_t>(kind)];
}

struct SceneryObject {
    SceneryKind kind;
    u32 variant;
    Vec2 position;
    f32 scale;
    Color color;
    i32 tile;
    bool active = true;
    u8 facing = 0; // world orientation in clockwise quarter turns
};

// Shared by mesh placement and physical footprints, including natural rocks.
f32 scenery_heading(const SceneryObject& object);

// A horizontal capsule extruded vertically: round trunks and rounded rock
// footprints, with a finite height. a == b is a circular footprint.
struct ObjectCollider {
    Vec2 a, b;
    f32 radius = 0;
    f32 bottom = 0, top = 0;  // logical terrain height, in tiles
    u32 object = 0;
};

struct ObjectContact {
    f32 time = 1.0f;  // fraction of the requested displacement
    Vec2 normal;
    u32 collider = 0;
};

class Scenery {
public:
    std::vector<SceneryObject> objects;
    std::vector<ObjectCollider> colliders;
    bool ready = false;

    // Rebuild after editing colliders. A compact uniform grid keeps queries
    // local even on maps holding hundreds of thousands of scenery objects.
    void rebuild_index(i32 width, i32 height);
    bool overlaps(Vec2 position, f32 radius, f32 bottom, f32 height) const;
    bool sweep(Vec2 from, Vec2 displacement, f32 radius, f32 bottom, f32 height, ObjectContact& hit,
               u32 ignored_object = ~0u) const;
    Vec2 slide(Vec2 from, Vec2 displacement, f32 radius, f32 bottom, f32 height) const;

private:
    static constexpr f32 kCellSize = 4.0f;
    i32 columns_ = 0, rows_ = 0;
    std::vector<u32> offsets_, indices_;
    void cells(Vec2 low, Vec2 high, i32& x0, i32& y0, i32& x1, i32& y1) const;
};

// Retains the original seeded placement, variants, size and tint. Used for
// generation; runtime harvesting edits existing objects and rebuilds colliders.
Scenery build_scenery(const TileMap& map);
void rebuild_scenery_colliders(TileMap& map);
f32 collision_floor(const TileMap& map, Vec2 position);
bool scenery_blocks(SceneryKind kind);

}  // namespace zl
