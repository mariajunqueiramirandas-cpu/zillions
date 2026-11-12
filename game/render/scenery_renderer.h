// Instanced static GLB scenery. Spatial buckets bound submission work by the
// visible area, rather than the area of a multi-kilometre world.
#pragma once

#include <array>
#include <functional>
#include <vector>

#include "asset/gltf.h"
#include "gfx/isometric_camera.h"
#include "gfx/shader.h"
#include "gfx/shadows.h"
#include "gfx/texture.h"
#include "world/scenery.h"

namespace zl {

class SceneryRenderer {
public:
    ~SceneryRenderer();
    void init(const char* asset_dir);
    bool has_model(SceneryKind kind) const;
    void set_scene(const Scenery& scene, i32 width, i32 height,
                   const std::function<f32(Vec2)>& ground_height);
    u32 instance_count() const { return static_cast<u32>(placements_.size()); }
    u32 model_count() const;
    f32 max_height() const { return max_height_; }
    u32 draw(const IsometricCamera& camera, const ShadowView* shadows, f32 depth_scale,
             f32 begin, f32 end, const Vec2* reveal_lo, const Vec2* reveal_hi,
             u32 height_texture, f32 mesh_step);
    void draw_shadow(const ShadowView& shadow, u32 height_texture, f32 mesh_step);
    bool draw_preview(const IsometricCamera& camera, Vec2 point, f32 height, bool valid,
                      u8 facing, f32 depth_scale, u32 height_texture, f32 mesh_step);

private:
    struct Instance { Vec3 position; f32 scale; Vec2 rotation; Color color; };
    struct Placement { Instance instance; SceneryKind kind; };
    struct Mesh { u32 vao = 0, vertices = 0, indices = 0, count = 0; i32 material = 0; };
    struct Material {
        Texture texture, metallic_roughness;
        f32 factor[4] = {1, 1, 1, 1};
        f32 metallic = 0, roughness = 1;
    };
    struct Model { std::vector<Mesh> meshes; std::vector<Material> materials; };
    static constexpr i32 kKinds = static_cast<i32>(SceneryKind::Count);
    static constexpr i32 kBucketSize = 32;
    std::array<Model, kKinds> models_;
    std::array<std::vector<Instance>, kKinds> visible_;
    std::vector<Placement> placements_;
    std::vector<std::vector<u32>> buckets_;
    i32 columns_ = 0, rows_ = 0, width_ = 0, height_ = 0;
    f32 max_ground_ = 0, max_height_ = 0, max_radius_ = 0;
    Shader shader_;
    u32 instance_buffer_ = 0;
    bool shadow_pass_ = false;
    void clear_models();
    void gather(Vec2 low, Vec2 high, const std::function<bool(const Placement&)>& accept);
    void bind_heightfield(u32 texture, f32 step);
    void bind_view(const IsometricCamera& camera, const ShadowView* shadows, f32 depth_scale,
                   const Vec2* reveal_lo, const Vec2* reveal_hi, u32 height_texture, f32 step);
    void submit();
};

} // namespace zl
