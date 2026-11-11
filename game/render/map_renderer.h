// Draws a height-displaced isometric world and depth-tested upright scenery.
#pragma once

#include <vector>

#include "core/base.h"
#include "gfx/isometric_camera.h"
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
    bool shadows = true;
    bool collisions = false;  // F3: show the physical object volumes
    MapOverlay overlay = MapOverlay::None;
};

struct SeeThrough { Vec2 lo, hi; };  // bounds in the projected isometric plane

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
    // Terrain heights are unchanged by harvesting: refresh ids, scenery and
    // shadow casters without regenerating expensive mountain relief.
    void refresh_objects(const TileMap& map);
    void draw_chest_preview(const IsometricCamera& camera, Vec2 point, bool valid, u8 facing = 0);
    void draw(const IsometricCamera& camera, const MapDrawOptions& options);
    void draw_ground(const IsometricCamera& camera, const MapDrawOptions& options);
    void draw_scenery(const IsometricCamera& camera, f32 depth_begin, f32 depth_end, const SeeThrough* reveal = nullptr);
    void draw_collisions(const IsometricCamera& camera);
    const ShadowView* shadow_view() const { return active_shadows_ ? &shadow_view_ : nullptr; }
    f32 depth_scale() const { return 1.0f / (static_cast<f32>(map_width_ + map_height_) + height_range_ * tiles_per_unit_ + 32.0f); }
    f32 ground_height(Vec2 position) const;
    f32 surface_height(Vec2 position, f32 zoom) const;
    // Closest visible intersection, including elevated terrain. False when
    // the cursor looks past the map's silhouette.
    bool pick(const IsometricCamera& camera, Vec2 screen, Vec2& world) const;
    void frame_map(IsometricCamera& camera) const;

    // The sprite atlas and its catalogue, for anything else that draws sprites.
    const Texture& sprite_atlas() const { return sprites_; }
    const TerrainArt& art() const { return art_; }

    u32 decor_count() const { return static_cast<u32>(decor_.size()); }
    u32 sprites_drawn() const { return sprites_drawn_; }

private:
    void build_decor(const TileMap& map);
    f32 mesh_step(f32 zoom) const;
    void update_shadows(const IsometricCamera& camera);

    TerrainArt art_;
    Shader terrain_;
    Shader terrain_depth_, scenery_depth_, solid_depth_;
    Shader collision_lines_;
    u32 collision_vao_ = 0, collision_buffer_ = 0;
    std::vector<ObjectCollider> collision_geometry_;
    ShadowView shadow_view_;
    u32 shadow_fbo_ = 0, shadow_texture_ = 0, caster_vao_ = 0, caster_buffer_ = 0;
    u32 solid_vao_ = 0, solid_buffer_ = 0, shadow_depth_sampler_ = 0;
    bool shadows_valid_ = false;
    bool active_shadows_ = true;
    i32 shadow_rotation_ = -1;
    f32 shadow_step_ = 0.0f;
    i32 u_view_ = -1, u_rotation_ = -1, u_depth_scale_ = -1, u_step_ = -1, u_cells_ = -1, u_wall_ = -1;
    i32 u_map_size_ = -1, u_time_ = -1, u_grid_ = -1, u_overlay_ = -1, u_pixel_ = -1, u_unit_ = -1;
    f32 height_range_ = 1.0f, tiles_per_unit_ = 1.0f;
    u32 vao_ = 0;
    u32 chunk_buffer_ = 0;

    Texture ground_;    // terrain texture array
    Texture sprites_;   // sprite atlas
    Texture tiles_;     // per-tile ids, point sampled
    Texture relief_;    // surface normal, height and cavity at kReliefTexelsPerUnit texels per world unit
    Texture surface_;   // mountain altitude, cliff faces and height level, same resolution
    Texture depth_;     // per-tile water depth and moisture, filtered
    Texture heights_;   // R32F geometry, in tiles
    std::vector<f32> heightfield_;
    i32 height_width_ = 0, height_height_ = 0;

    struct Chunk { Vec2 origin; f32 low, high; };
    std::vector<Chunk> chunks_;
    std::vector<Vec2> visible_chunks_;

    SpriteBatch batch_;
    struct Decor { Vec2 ground; f32 height; SpriteInstance sprite; bool casts_shadow; i32 directional = -1; u8 facing = 0; SceneryKind kind = SceneryKind::Tuft; f32 scale = 1; };
    SpriteInstance view_sprite(const Decor& decor, i32 rotation) const;
    std::vector<Decor> decor_;
    std::vector<u32> decor_order_[4];  // stable back-to-front order for each view
    i32 map_width_ = 0, map_height_ = 0;
    u32 sprites_drawn_ = 0;
};

}  // namespace zl
