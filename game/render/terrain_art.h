// Art for the map: ground textures and a sprite atlas for trees, rocks and
// small ground cover. The painted art is read from PNG files laid out as
// docs/art-direction.md describes. Every piece also has a placeholder drawn in
// code, used when the files are not there, so the game runs without its assets.
#pragma once

#include <vector>

#include "core/base.h"
#include "core/image.h"
#include "world/tilemap.h"

namespace zl {

class JobSystem;

// Pixels per tile that the art is authored at.
inline constexpr i32 kArtTilePixels = 32;
// Each ground texture repeats every this many tiles.
inline constexpr i32 kGroundRepeatTiles = 8;
// Keep sprites isolated through mip level 4 (1/16 scale). Sprites disappear
// below 2.5 px/tile, before a smaller mip could add useful detail.
inline constexpr i32 kSpriteAtlasMipLevel = 4;
inline constexpr i32 kSpriteAtlasPadding = 1 << kSpriteAtlasMipLevel;
// Extra texture-array layer holding tileable noise for the terrain shader.
inline constexpr i32 kNoiseLayer = kTerrainCount;

struct SpriteDef {
    u16 u0, v0, u1, v1;  // atlas rectangle, normalised to 0..65535
    f32 width, height;   // size in tiles
    f32 anchor_x, anchor_y;  // point inside the sprite (in tiles from its top-left) that sits on the ground position
};

struct SpriteSet {
    i32 first = 0;
    i32 count = 0;
};

struct TerrainArt {
    std::vector<Image> ground;  // kTerrainCount terrain layers followed by the noise layer
    Image mountain_parameters; // linear data: ambient occlusion, roughness, metallic
    Image mountain_height;     // linear displacement, used for surface-gradient detail
    Image sprites;              // premultiplied alpha
    std::vector<SpriteDef> defs;

    SpriteSet broadleaf, conifer, bush;
    SpriteSet stone, iron, gold;
    SpriteSet tuft, flower, pebble, reed, boulder;
    SpriteSet branch, stump, rubble, chest;

    // Whether each group was read from files or drawn in code.
    bool painted_ground = false;
    bool painted_sprites = false;
};

// Reads <asset_dir>/ground/<terrain>.png and <asset_dir>/sprites/<family>_<n>.png.
// The ground textures and the sprites are each taken from files only if the
// whole group is present at one shared square resolution (256..2048); otherwise that group is
// drawn in code and the reason is logged. The water and noise layers are data
// for the terrain shader and are always generated. `seed` only affects what is
// drawn in code.
TerrainArt load_terrain_art(JobSystem& jobs, const char* asset_dir, u32 seed = 7);

// Draws everything in code: load_terrain_art() without an asset directory.
TerrainArt build_terrain_art(JobSystem& jobs, u32 seed = 7);

}  // namespace zl
