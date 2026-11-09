// Procedural map generation.
#pragma once

#include "core/base.h"
#include "world/tilemap.h"

namespace zl {

class JobSystem;

struct MapGenParams {
    u64 seed = 1;
    i32 size = 512;         // maps are square, size x size tiles
    // Tiles per world unit: 1, 2 or 4. Lakes, ranges, rivers and every other
    // feature are sized in world units, so raising the resolution keeps the
    // geography and divides it into a finer grid: more tiles across every
    // pass, shore and plateau, and room for more units in the same place.
    i32 resolution = 2;
    f32 water = 0.11f;      // fraction of the map covered by lakes
    f32 mountains = 0.13f;  // fraction of the map covered by mountains
    f32 forest = 0.27f;     // fraction of open land covered by trees
    f32 uplands = 0.42f;    // fraction of open land raised above the lowlands; 0 gives a flat map
    f32 rivers = 1.0f;      // river density multiplier; 0 disables rivers
    f32 ore = 1.0f;         // ore deposit density multiplier
    // Density multiplier for small features: copses, lone trees, boulder
    // outcrops and ponds. These are sized in tiles, not world units, so a
    // screenful of map holds about the same amount of them at any resolution.
    f32 detail = 1.0f;
};

// Generates a complete map. Deterministic: the same parameters always produce
// the same map, regardless of how many threads the job system has.
TileMap generate_map(const MapGenParams& params, JobSystem& jobs);

}  // namespace zl
