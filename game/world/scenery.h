// Static scenery shared by rendering and character collision. Positions and
// footprints are in simulation coordinates, independent of camera and zoom.
#pragma once

#include <vector>

#include "core/image.h"
#include "math/vec.h"

namespace zl {
struct TileMap;

enum class SceneryKind : u8 { Broadleaf, Conifer, Bush, Stone, Iron, Gold, Boulder, Tuft, Flower, Pebble, Reed };

struct SceneryObject {
    SceneryKind kind;
    u32 variant;
    Vec2 position;
    f32 scale;
    Color color;
    i32 tile;
};

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
    bool sweep(Vec2 from, Vec2 displacement, f32 radius, f32 bottom, f32 height, ObjectContact& hit) const;
    Vec2 slide(Vec2 from, Vec2 displacement, f32 radius, f32 bottom, f32 height) const;

private:
    static constexpr f32 kCellSize = 4.0f;
    i32 columns_ = 0, rows_ = 0;
    std::vector<u32> offsets_, indices_;
    void cells(Vec2 low, Vec2 high, i32& x0, i32& y0, i32& x1, i32& y1) const;
};

// Retains the original seeded placement, variants, size and tint. Call after
// terrain edits, regeneration, harvesting or replacing scenery objects.
Scenery build_scenery(const TileMap& map);
f32 collision_floor(const TileMap& map, Vec2 position);
bool scenery_blocks(SceneryKind kind);

}  // namespace zl
