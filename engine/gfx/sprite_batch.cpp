#include "gfx/sprite_batch.h"

#include <cstddef>

#include "gfx/gl.h"

namespace zl {

namespace {

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec4 aRect;
layout(location = 1) in vec4 aUV;
layout(location = 2) in vec4 aColor;
uniform vec4 uView;  // xy: clip scale, zw: clip offset
out vec2 vUV;
out vec4 vColor;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 world = aRect.xy + aRect.zw * corner;
    vUV = mix(aUV.xy, aUV.zw, corner);
    vColor = aColor;
    gl_Position = vec4(world * uView.xy + uView.zw, 0.0, 1.0);
}
)";

// Textures hold premultiplied alpha; the tint's alpha fades the whole sprite.
const char* kFragment = R"(#version 330 core
uniform sampler2D uAtlas;
in vec2 vUV;
in vec4 vColor;
out vec4 oColor;
void main() {
    vec4 texel = texture(uAtlas, vUV);
    oColor = texel * vec4(vColor.rgb * vColor.a, vColor.a);
}
)";

}  // namespace

SpriteBatch::~SpriteBatch() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void SpriteBatch::init() {
    shader_.compile("sprite", kVertex, kFragment);
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

void SpriteBatch::draw(u32 count, const Camera2D& camera, const Texture& atlas) {
    draw(count, camera, atlas.id());
}

void SpriteBatch::draw(u32 count, const Camera2D& camera, u32 atlas_texture) {
    ZL_ASSERT(mapped_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glUnmapBuffer(GL_ARRAY_BUFFER);
    mapped_ = false;
    if (count == 0) return;

    shader_.bind();
    const Vec2 scale = camera.clip_scale(), offset = camera.clip_offset();
    glUniform4f(u_view_, scale.x, scale.y, offset.x, offset.y);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_texture);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
    glBindVertexArray(0);
}

}  // namespace zl
