// The static world: terrain, resources and the scalar fields they were derived
// from. Stored as parallel arrays (structure of arrays) so systems that only
// need one attribute touch only that memory.
#pragma once

#include <vector>

#include "core/base.h"
#include "math/vec.h"

namespace zl {

enum class Terrain : u8 {
    Water,     // lakes and rivers; blocks movement and construction
    Shallows,  // fords: walkable (slowly), not buildable
    Sand,
    Grass,
    Dirt,
    Forest,    // trees block movement; harvested from adjacent tiles
    Mountain,
    Count
};

enum class Resource : u8 { None, Stone, Iron, Gold, Count };

namespace TileFlag {
enum : u8 {
    Walkable = 1 << 0,
    Buildable = 1 << 1,
    River = 1 << 2,  // water or ford that belongs to a river rather than a lake
};
}

inline constexpr int kTerrainCount = static_cast<int>(Terrain::Count);
inline constexpr int kResourceCount = static_cast<int>(Resource::Count);

const char* terrain_name(Terrain t);
const char* resource_name(Resource r);

struct MapStats {
    i32 terrain_tiles[kTerrainCount] = {};
    i32 deposits[kResourceCount] = {};   // number of deposit clusters per resource
    i32 river_tiles = 0;
    i32 fords = 0;
    i32 carved_tiles = 0;                // tiles altered to connect isolated regions
    i32 walkable_tiles = 0;
    i32 reachable_tiles = 0;             // walkable tiles connected to the start
    i32 reachable_border_tiles = 0;      // map-edge tiles the horde can enter from
    i32 buildable_near_start = 0;
    f64 gen_ms = 0.0;
    struct Phase {
        const char* name;
        f64 ms;
    };
    std::vector<Phase> phases;           // time spent in each generation step
};

struct TileMap {
    i32 width = 0;
    i32 height = 0;
    u64 seed = 0;
    IVec2 start;               // command centre location
    f32 sea_level = 0.0f;      // elevation below which tiles are lake
    f32 mountain_level = 1.0f; // elevation above which tiles are mountain

    std::vector<Terrain> terrain;
    std::vector<Resource> resource;
    std::vector<u8> flags;
    std::vector<f32> elevation;   // [0, 1]
    std::vector<f32> moisture;    // [0, 1]
    std::vector<u8> shore_dist;   // water tiles: distance to land in quarter tiles; land: 0

    MapStats stats;

    i32 tile_count() const { return width * height; }
    i32 index(i32 x, i32 y) const { return y * width + x; }
    bool in_bounds(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    bool walkable(i32 x, i32 y) const { return (flags[static_cast<size_t>(index(x, y))] & TileFlag::Walkable) != 0; }
    bool buildable(i32 x, i32 y) const { return (flags[static_cast<size_t>(index(x, y))] & TileFlag::Buildable) != 0; }
};

}  // namespace zl
