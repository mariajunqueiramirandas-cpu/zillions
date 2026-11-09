// Instanced sprite renderer. Every sprite is one 28-byte instance; the quad
// corners are derived from gl_VertexID, so there is no vertex buffer at all.
// A whole batch, whatever its size, is a single draw call.
#pragma once

#include "core/base.h"
#include "gfx/camera2d.h"
#include "gfx/shader.h"
#include "gfx/texture.h"

namespace zl {

struct SpriteInstance {
    f32 x, y;         // world position of the top-left corner
    f32 w, h;         // world size
    u16 u0, v0;       // atlas rectangle, normalised to 0..65535
    u16 u1, v1;
    u32 color;        // RGBA8 tint (byte order R, G, B, A), multiplied with the texel
};
static_assert(sizeof(SpriteInstance) == 28, "SpriteInstance layout is part of the vertex format");

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

    // Returns write-only GPU memory with room for `capacity` instances. Fill it
    // front to back, then call draw() with the number actually written. The
    // pointer is invalid after draw().
    SpriteInstance* begin(u32 capacity);

    // Uploads nothing (the data is already in GPU memory) and issues one
    // instanced draw. Sprites are drawn in array order with premultiplied-alpha
    // blending, so write them back to front.
    void draw(u32 count, const Camera2D& camera, const Texture& atlas);

private:
    Shader shader_;
    i32 u_view_ = -1;
    u32 vao_ = 0;
    u32 vbo_ = 0;
    u32 capacity_ = 0;
    bool mapped_ = false;
};

}  // namespace zl
