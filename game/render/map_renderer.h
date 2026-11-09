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

struct MapDrawOptions {
    f32 time = 0.0f;      // seconds; animates water
    bool grid = false;    // overlay tile boundaries
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

    u32 decor_count() const { return static_cast<u32>(decor_.size()); }
    u32 sprites_drawn() const { return sprites_drawn_; }

private:
    void build_decor(const TileMap& map);

    TerrainArt art_;
    Shader terrain_;
    i32 u_world_ = -1, u_map_size_ = -1, u_time_ = -1, u_grid_ = -1, u_pixel_ = -1, u_mountain_height_ = -1, u_height_range_ = -1;
    f32 mountain_height_ = 1.0f, height_range_ = 1.0f;
    u32 vao_ = 0;

    Texture ground_;    // terrain texture array
    Texture sprites_;   // sprite atlas
    Texture tiles_;     // per-tile ids, point sampled
    Texture relief_;    // surface normal, height and cavity at kReliefScale texels per tile
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
