// Draws a TileMap: one full-screen shader pass for the ground and one
// instanced batch for everything standing on it.
#pragma once

#include <vector>

#include "core/base.h"
#include "gfx/camera2d.h"
#include "gfx/shader.h"
#include "gfx/sprite_batch.h"
#include "gfx/texture.h"
#include "render/terrain_art.h"
#include "world/tilemap.h"

namespace zl {

class JobSystem;

// Debug views of the gameplay data, drawn per tile over the terrain.
enum class MapOverlay : u8 {
    None,
    Height,       // height level of each tile, ramps and cliff edges
    Passability,  // buildable, walkable only, ramp or blocked
    Count
};

struct MapDrawOptions {
    f32 time = 0.0f;      // seconds; animates water
    bool grid = false;    // overlay tile boundaries
    MapOverlay overlay = MapOverlay::None;
};

class MapRenderer {
public:
    MapRenderer() = default;
    ~MapRenderer();
    MapRenderer(const MapRenderer&) = delete;
    MapRenderer& operator=(const MapRenderer&) = delete;

    void init(JobSystem& jobs);
    void set_map(const TileMap& map, JobSystem& jobs);
    void draw(const Camera2D& camera, const MapDrawOptions& options);

    // The sprite atlas and its catalogue, for anything else that draws sprites.
    const Texture& sprite_atlas() const { return sprites_; }
    const TerrainArt& art() const { return art_; }

    u32 decor_count() const { return static_cast<u32>(decor_.size()); }
    u32 sprites_drawn() const { return sprites_drawn_; }

private:
    void build_decor(const TileMap& map);

    TerrainArt art_;
    Shader terrain_;
    i32 u_world_ = -1, u_map_size_ = -1, u_time_ = -1, u_grid_ = -1, u_overlay_ = -1, u_pixel_ = -1, u_height_range_ = -1, u_unit_ = -1;
    f32 height_range_ = 1.0f, tiles_per_unit_ = 1.0f;
    u32 vao_ = 0;

    Texture ground_;    // terrain texture array
    Texture sprites_;   // sprite atlas
    Texture tiles_;     // per-tile ids, point sampled
    Texture relief_;    // surface normal, height and cavity at kReliefTexelsPerUnit texels per world unit
    Texture surface_;   // mountain altitude, cliff faces and height level, same resolution
    Texture depth_;     // per-tile water depth and moisture, filtered

    SpriteBatch batch_;
    // Static scenery, sorted north to south so later sprites overlap earlier
    // ones. row_start_[y] is the first sprite standing on tile row y.
    std::vector<SpriteInstance> decor_;
    std::vector<u32> row_start_;
    i32 map_width_ = 0, map_height_ = 0;
    u32 sprites_drawn_ = 0;
};

}  // namespace zl
