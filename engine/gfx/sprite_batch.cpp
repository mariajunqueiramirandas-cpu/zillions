#include "gfx/sprite_batch.h"

#include <cstddef>
#include <string>

#include "gfx/gl.h"

namespace zl {

namespace {

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec4 aRect;
layout(location = 1) in vec4 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aDepth;
uniform vec4 uView;  // xy: clip scale, zw: clip offset
out vec2 vUV;
out vec4 vColor;
flat out vec4 vDepth;
out float vY;
out vec2 vPlane;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 world = aRect.xy + aRect.zw * corner;
    vUV = mix(aUV.xy, aUV.zw, corner);
    vColor = aColor;
    vDepth = aDepth;
    vY = corner.y;
    vPlane = world;
    gl_Position = vec4(world * uView.xy + uView.zw, aDepth.x, 1.0);
}
)";

// Textures hold premultiplied alpha; the tint's alpha fades the whole sprite.
const char* kFragment = R"(
uniform sampler2D uAtlas;
uniform mat2 uInverseRotation;
uniform float uWorldDepthScale;
in vec2 vUV;
in vec4 vColor;
flat in vec4 vDepth;
in float vY;
in vec2 vPlane;
out vec4 oColor;
void main() {
    vec4 texel = texture(uAtlas, vUV);
    // Above the anchor this is an upright object; below it the painted contact
    // shadow lies on the ground. Both must compare correctly with terrain.
    float delta = vY - vDepth.z;
    float depth = vDepth.x + delta * (delta < 0.0 ? vDepth.y : vDepth.w);
    gl_FragDepth = depth * 0.5 + 0.5;
    oColor = texel * vec4(vColor.rgb * vColor.a, vColor.a);
    if (uShadows) {
        // Recover the same 3D upright/ground position used by depth testing.
        float height = (-depth / uWorldDepthScale - vPlane.y * 1.732050808) * 0.5;
        float sum = (vPlane.y + height * 0.866025404) / 0.353553391;
        float difference = vPlane.x / 0.707106781;
        vec2 position = uInverseRotation * vec2(sum + difference, sum - difference) * 0.5;
        // A billboard's plane can extend below ground when extrapolated across
        // the PCF kernel. Do not use terrain's receiver-plane correction here.
        float visibility = shadowVisibility(position, height, false);
        oColor.rgb *= mix(vec3(0.53, 0.58, 0.65), vec3(1.0), visibility);
    }
    if (texel.a < 0.002) discard;
}
)";

}  // namespace

SpriteBatch::~SpriteBatch() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void SpriteBatch::init() {
    const std::string fragment = std::string("#version 330 core\n") + kShadowSamplingGLSL + kFragment;
    shader_.compile("sprite", kVertex, fragment.c_str());
    shader_.bind();
    glUniform1i(shader_.uniform("uAtlas"), 0);
    u_view_ = shader_.uniform("uView");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    const GLsizei stride = sizeof(SpriteInstance);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SpriteInstance, x)));
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_SHORT, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(SpriteInstance, u0)));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(SpriteInstance, color)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(SpriteInstance, depth)));
    glVertexAttribDivisor(3, 1);
    glBindVertexArray(0);
}

SpriteInstance* SpriteBatch::begin(u32 capacity) {
    ZL_ASSERT(!mapped_);
    if (capacity == 0) capacity = 1;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (capacity > capacity_) {
        // Grow geometrically so steady-state frames never reallocate.
        capacity_ = capacity + capacity / 2;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity_) * sizeof(SpriteInstance), nullptr, GL_STREAM_DRAW);
    }
    // Invalidating the buffer lets the driver hand back fresh memory instead of
    // stalling until the previous frame's draw has finished reading it.
    void* memory = glMapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(capacity) * sizeof(SpriteInstance),
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    ZL_CHECK(memory != nullptr, "could not map the sprite buffer");
    mapped_ = true;
    return static_cast<SpriteInstance*>(memory);
}

void SpriteBatch::draw(u32 count, const Camera2D& camera, const Texture& atlas,
                       const ShadowView* shadows, const IsometricCamera* world_camera, f32 depth_scale) {
    ZL_ASSERT(mapped_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glUnmapBuffer(GL_ARRAY_BUFFER);
    mapped_ = false;
    if (count == 0) return;

    shader_.bind();
    const Vec2 scale = camera.clip_scale(), offset = camera.clip_offset();
    glUniform4f(u_view_, scale.x, scale.y, offset.x, offset.y);
    bind_shadows(shader_, shadows);
    if (shadows && world_camera) {
        const Vec2 x = world_camera->unrotate({1, 0}), y = world_camera->unrotate({0, 1});
        const f32 inverse[] = {x.x, x.y, y.x, y.y};
        glUniformMatrix2fv(shader_.uniform("uInverseRotation"), 1, GL_FALSE, inverse);
        glUniform1f(shader_.uniform("uWorldDepthScale"), depth_scale);
    }
    atlas.bind(0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
    glBindVertexArray(0);
}

}  // namespace zl
