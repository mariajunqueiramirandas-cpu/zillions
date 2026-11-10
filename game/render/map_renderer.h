// Draws a TileMap: one full-screen shader pass for the ground and an
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

// A box on the map (in tiles) that must stay visible: tall scenery drawn in
// front of it turns translucent where it would cover it.
struct SeeThrough {
    Vec2 lo, hi;
};

class MapRenderer {
public:
    MapRenderer() = default;
    ~MapRenderer();
    MapRenderer(const MapRenderer&) = delete;
    MapRenderer& operator=(const MapRenderer&) = delete;

    // `asset_dir` is where the painted art lives (see load_terrain_art); null
    // draws placeholder art in code.
    void init(JobSystem& jobs, const char* asset_dir = nullptr);
    // Canvas sizes and atlas slots are fixed for both art sources, so scenery
    // keeps its UVs and the map/camera need not be rebuilt when art changes.
    void reload_art(JobSystem& jobs, const char* asset_dir);
    void set_map(const TileMap& map, JobSystem& jobs);
    // Ground and all scenery: two draw calls.
    void draw(const Camera2D& camera, const MapDrawOptions& options);

    // The same in pieces, for drawing other things among the scenery. Scenery
    // is drawn north to south so that nearer things overlap further ones;
    // draw_scenery() draws what stands at y_begin <= y < y_end, so anything
    // drawn between two calls sits at that depth in the scene.
    void draw_ground(const Camera2D& camera, const MapDrawOptions& options);
    void draw_scenery(const Camera2D& camera, f32 y_begin, f32 y_end, const SeeThrough* reveal = nullptr);

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
    std::vector<f32> decor_y_;  // where each stands on the ground, ascending
    std::vector<u32> row_start_;
    i32 map_width_ = 0, map_height_ = 0;
    u32 sprites_drawn_ = 0;
};

}  // namespace zl
