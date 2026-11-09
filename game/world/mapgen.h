// Procedural map generation.
#pragma once

#include "core/base.h"
#include "world/tilemap.h"

namespace zl {

class JobSystem;

struct MapGenParams {
    u64 seed = 1;
    i32 size = 256;         // maps are square, size x size tiles
    f32 water = 0.11f;      // fraction of the map covered by lakes
    f32 mountains = 0.13f;  // fraction of the map covered by mountains
    f32 forest = 0.27f;     // fraction of open land covered by trees
    f32 uplands = 0.42f;    // fraction of open land raised above the lowlands; 0 gives a flat map
    f32 rivers = 1.0f;      // river density multiplier; 0 disables rivers
    f32 ore = 1.0f;         // ore deposit density multiplier
};

// Generates a complete map. Deterministic: the same parameters always produce
// the same map, regardless of how many threads the job system has.
TileMap generate_map(const MapGenParams& params, JobSystem& jobs);

}  // namespace zl
