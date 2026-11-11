// Draws the plague doctor and the tool in its right hand in the isometric
// world: skinned vertices, the scene's sun and shadow map, the same depth
// convention as the terrain and sprites, a contact shadow and the far-zoom
// marker. The character writes depth so southern scenery occludes it.
#pragma once

#include <vector>

#include "actor/plague.h"
#include "asset/gltf.h"
#include "gfx/isometric_camera.h"
#include "gfx/shader.h"
#include "gfx/shadows.h"
#include "gfx/texture.h"
#include "math/mat.h"

namespace zl {

struct PlagueDraw {
    Vec2 position{0, 0};      // ground point in tiles
    f32 ground_height = 0;    // terrain surface in tiles
    f32 heading = 0;          // radians; 0 east, pi/2 south
    f32 wade = 0;             // metres of water stood in
    f32 marker = 0;           // 0..1 far-zoom ring
    HumanTool tool = HumanTool::None;
    bool show_tool = false;
};

class ModelRenderer {
public:
    ModelRenderer() = default;
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    // `asset_dir` is the root that holds models/plague_character.glb, axe.glb
    // and pickaxe.glb. A missing model leaves the renderer not ready and the
    // caller falls back to the procedural figure.
    void init(const char* asset_dir);
    bool ready() const { return ready_; }
    const GltfModel* model() const { return ready_ ? &plague_ : nullptr; }

    // Draws the contact shadow, the character and the tool. Call between the
    // northern and southern scenery slices, like the painted figure.
    void draw(const IsometricCamera& camera, const ShadowView* shadows, f32 depth_scale,
              const PlagueDraw& item, const PlaguePose& pose);
    // Screen-space ring, drawn after the scenery so it stays visible over trees.
    void draw_marker(const IsometricCamera& camera, const PlagueDraw& item);
    // Projected bounds for the tree-reveal fade.
    bool reveal_bounds(const IsometricCamera& camera, const PlagueDraw& item, Vec2& lo, Vec2& hi) const;

    u32 draw_calls() const { return draw_calls_; }

private:
    struct GpuMesh {
        u32 vao = 0, vbo = 0, ebo = 0;
        u32 count = 0;
        i32 material = -1;
    };
    struct GpuMaterial {
        Texture texture;
        f32 factor[4] = {1, 1, 1, 1};
    };

    void load(const char* asset_dir);
    void upload(const GltfModel& model, std::vector<GpuMesh>& meshes, std::vector<GpuMaterial>& materials, bool tool);
    void draw_mesh(const GpuMesh& mesh, const GpuMaterial& material, const Mat4& model_matrix,
                   const Mat4* bones, u32 bone_count, bool skinned, f32 waterline);
    void draw_contact_shadow(const IsometricCamera& camera, f32 depth_scale, const PlagueDraw& item);

    GltfModel plague_, axe_, pickaxe_;
    std::vector<GpuMesh> plague_meshes_, axe_meshes_, pickaxe_meshes_;
    std::vector<GpuMaterial> plague_materials_, axe_materials_, pickaxe_materials_;
    Shader shader_, shadow_shader_, marker_shader_;
    u32 shadow_vao_ = 0, marker_vao_ = 0;
    i32 u_view_ = -1, u_rotation_ = -1, u_depth_scale_ = -1, u_model_ = -1, u_bones_ = -1;
    i32 u_skinned_ = -1, u_base_color_factor_ = -1, u_waterline_ = -1;
    i32 u_view_dir_ = -1, u_sun_ = -1;
    bool ready_ = false;
    u32 draw_calls_ = 0;
};

}  // namespace zl
