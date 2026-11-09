// Procedurally generated placeholder art: ground textures and a sprite atlas
// for trees, rocks and small ground cover. Everything is drawn in code at
// start-up, so the game ships no image files yet and each asset can be swapped
// for hand-made art later without touching the renderer.
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
    Image sprites;              // premultiplied alpha
    std::vector<SpriteDef> defs;

    SpriteSet broadleaf, conifer, bush;
    SpriteSet stone, iron, gold;
    SpriteSet tuft, flower, pebble, reed;
};

TerrainArt build_terrain_art(JobSystem& jobs, u32 seed = 7);

}  // namespace zl
