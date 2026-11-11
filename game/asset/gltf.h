// A focused glTF 2.0 / GLB reader for the game's character and tool models.
// It keeps the parts the renderer needs: indexed primitives with skinning
// attributes, node transforms, skins, linear/step animation samplers and
// base-colour textures. Everything is CPU-side and headless so the loader and
// the animation state machine can be tested without a GPU.
#pragma once

#include <string>
#include <vector>

#include "core/image.h"
#include "math/mat.h"

namespace zl {

struct GltfVertex {
    Vec3 position;
    Vec3 normal{0, 0, 1};
    Vec2 uv;
    u16 joints[4] = {0, 0, 0, 0};
    f32 weights[4] = {0, 0, 0, 0};
};

struct GltfPrimitive {
    std::vector<GltfVertex> vertices;
    std::vector<u32> indices;
    i32 material = -1;
};

struct GltfMaterial {
    Image base_color;                       // decoded and downsampled RGBA8
    f32 base_color_factor[4] = {1, 1, 1, 1};
    bool double_sided = false;
};

struct GltfNode {
    std::string name;
    i32 parent = -1;
    Vec3 translation{0, 0, 0};
    Quat rotation;
    Vec3 scale{1, 1, 1};
    bool has_matrix = false;
    Mat4 matrix;
    std::vector<i32> children;
    i32 mesh = -1;
    i32 skin = -1;
};

struct GltfSkin {
    std::vector<i32> joints;
    std::vector<Mat4> inverse_bind;
};

enum class GltfPath : u8 { Translation, Rotation, Scale };

struct GltfChannel {
    i32 node = -1;
    GltfPath path = GltfPath::Translation;
    bool step = false;
    std::vector<f32> times;
    std::vector<Vec3> vec3;  // translation or scale keys
    std::vector<Quat> quat;  // rotation keys
};

struct GltfAnimation {
    std::string name;
    f32 duration = 0.0f;
    std::vector<GltfChannel> channels;

    const GltfChannel* channel(i32 node, GltfPath path) const;
};

struct GltfModel {
    std::vector<GltfPrimitive> primitives;
    std::vector<GltfMaterial> materials;
    std::vector<GltfNode> nodes;
    std::vector<GltfSkin> skins;
    std::vector<GltfAnimation> animations;
    i32 root = -1;
    i32 skin_index = -1;
    i32 hand_node = -1;  // mixamorig:RightHand when present
    bool skinned = false;
    Vec3 bounds_min{0, 0, 0};
    Vec3 bounds_max{0, 0, 0};

    // Reads a .glb (or a .gltf with embedded data URIs) and decodes only the
    // base-colour textures. Returns false and fills `error` on failure.
    bool load(const char* path, std::string& error, i32 max_texture_size = 1024);

    const GltfAnimation* animation(const char* name) const;
    i32 node_by_name(const char* name) const;
};

// Decodes PNG or JPEG bytes to RGBA8. Exposed for the loader tests.
bool decode_image(const u8* data, size_t size, Image& image);

// Box-filtered resize that preserves aspect ratio. Used to keep the 4K source
// textures at a sane runtime size.
Image downsample_image(const Image& image, i32 max_size);

}  // namespace zl
