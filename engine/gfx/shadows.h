// Shared directional-shadow projection and sampling for terrain and sprites.
#pragma once

#include "math/vec.h"

namespace zl {
class Shader;

struct ShadowView {
    u32 texture = 0;
    u32 depth_sampler = 0; // non-comparison sampler for blocker height
    Vec2 origin, size;
    // Horizontal direction towards the sun per unit of vertical rise. The
    // light is fixed in world space, independent of camera quarter turns.
    Vec2 light{-0.90f, -0.35f};
    f32 height = 1.0f;
    f32 softness = 0.30f;
    i32 resolution = 4096;
};

// Binds comparison texture unit 6 and raw depth unit 7, supplying the uniforms
// in kShadowSamplingGLSL.
void bind_shadows(const Shader& shader, const ShadowView* view);
extern const char* kShadowSamplingGLSL;
}  // namespace zl
