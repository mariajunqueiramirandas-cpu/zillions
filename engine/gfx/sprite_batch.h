// Instanced sprite renderer. Every sprite is one 48-byte instance; the quad
// corners are derived from gl_VertexID, so there is no vertex buffer at all.
// A whole batch, whatever its size, is a single draw call.
#pragma once

#include "core/base.h"
#include "gfx/camera2d.h"
#include "gfx/isometric_camera.h"
#include "gfx/shadows.h"
#include "gfx/shader.h"
#include "gfx/texture.h"

namespace zl {

inline constexpr f32 kSpriteDepthBias = 0.025f;

struct SpriteInstance {
    f32 x, y;         // world position of the top-left corner
    f32 w, h;         // world size
    u16 u0, v0;       // atlas rectangle, normalised to 0..65535
    u16 u1, v1;
    u32 color;        // RGBA8 tint (byte order R, G, B, A), multiplied with the texel
    f32 depth = 0.0f;         // clip-space depth at the ground anchor
    f32 depth_span = 0.0f;    // vertical object's depth slope, per sprite height
    f32 anchor = 0.0f;        // normalised vertical ground anchor
    f32 ground_span = 0.0f;   // ground-plane depth slope for the painted shadow
    f32 box_facing = -1.0f;   // 0..3: receive depth/shadows on an oriented box; -1: billboard
};
static_assert(sizeof(SpriteInstance) == 48, "SpriteInstance layout is part of the vertex format");

constexpr u32 pack_color(u8 r, u8 g, u8 b, u8 a = 255) {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
}

class SpriteBatch {
public:
    SpriteBatch() = default;
    ~SpriteBatch();
    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    void init();
    // Proxy dimensions for centered painted boxes: half width/depth and full
    // height. canvas_width is the unscaled sprite canvas width in world units.
    void set_box_dimensions(Vec3 dimensions, f32 canvas_width = 1.5f) { box_dimensions_=dimensions; box_canvas_width_=canvas_width; }

    // Returns write-only GPU memory with room for `capacity` instances. Fill it
    // back to front, then call draw() with the number actually written. The
    // pointer is invalid after draw().
    SpriteInstance* begin(u32 capacity);

    // Uploads nothing (the data is already in GPU memory) and issues one
    // instanced draw. Sprites are drawn in array order with premultiplied-alpha
    // blending, so write them back to front.
    void draw(u32 count, const Camera2D& camera, const Texture& atlas,
              const ShadowView* shadows = nullptr, const IsometricCamera* world_camera = nullptr, f32 depth_scale = 1.0f);
    void draw(u32 count, const Camera2D& camera, u32 atlas_texture,
              const ShadowView* shadows = nullptr, const IsometricCamera* world_camera = nullptr, f32 depth_scale = 1.0f);

private:
    Shader shader_;
    i32 u_view_ = -1;
    u32 vao_ = 0;
    u32 vbo_ = 0;
    u32 capacity_ = 0;
    bool mapped_ = false;
    Vec3 box_dimensions_{};
    f32 box_canvas_width_ = 1.5f;
};

}  // namespace zl
