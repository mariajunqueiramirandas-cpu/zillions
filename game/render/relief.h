// Builds the fine-grained height field the terrain shader lights the map with.
// Open country is terraced into height levels with cliffs and ramps between
// them; mountains get an eroded surface with branching ridges and valleys.
#pragma once

#include <vector>

#include "core/base.h"
#include "core/image.h"
#include "world/tilemap.h"

namespace zl {

class JobSystem;

// Relief texels per world unit (see MapGenParams::resolution).
inline constexpr i32 kReliefTexelsPerUnit = 4;

struct Relief {
    // r, g: surface normal x, y (packed to 0..255). b: height. a: cavity, i.e.
    // how far the surface sits above (>0.5) or below (<0.5) its surroundings.
    Image shape;
    // r: altitude within a mountain range (0 at its foot, 1 at the highest
    // summit). g: how much of a cliff face this point is. b: height level, as a
    // fraction of the highest level, sloping across ramps.
    Image surface;
    // Full precision geometry, in world units. Lighting's packed 8-bit height
    // is insufficient for displaced geometry and would leave visible bands.
    std::vector<f32> heights;
    f32 height_range = 1.0f;  // height in world units that shape.b == 1 stands for
};

Relief build_relief(const TileMap& map, JobSystem& jobs);

}  // namespace zl
