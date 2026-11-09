// Builds the fine-grained height field the terrain shader lights the map with.
// Open country gets a smooth upsampling of tile elevation; mountains get an
// eroded surface with branching ridges and valleys.
#pragma once

#include "core/base.h"
#include "core/image.h"
#include "world/tilemap.h"

namespace zl {

class JobSystem;

// Relief texels per tile.
inline constexpr i32 kReliefScale = 4;

struct Relief {
    // r, g: surface normal x, y (packed to 0..255). b: height. a: cavity, i.e.
    // how far the surface sits above (>0.5) or below (<0.5) its surroundings.
    Image texture;
    f32 height_range = 1.0f;   // height in tiles that b == 1 stands for
    f32 mountain_foot = 0.0f;  // normalised height at which mountains begin
};

Relief build_relief(const TileMap& map, JobSystem& jobs);

}  // namespace zl
