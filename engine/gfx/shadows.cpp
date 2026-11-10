#include "gfx/shadows.h"

#include "gfx/gl.h"
#include "gfx/shader.h"

namespace zl {

const char* kShadowSamplingGLSL = R"(
uniform sampler2DShadow uShadowMap;
uniform vec4 uShadowBounds;
uniform vec2 uShadowLight;
uniform float uShadowHeight;
uniform float uShadowSoftness;
uniform bool uShadows;

float shadowVisibility(vec2 position, float height, bool receiverPlane) {
    if (!uShadows) return 1.0;
    vec2 uv = (position - uShadowLight * height - uShadowBounds.xy) / uShadowBounds.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    float reference = 1.0 - height / uShadowHeight;

    // Receiver-plane correction keeps the soft filter from shadowing the
    // surface's own steep mountain slope. A small world-space bias anchors
    // contacts without acne or a visible detached gap at tree roots.
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    float zx = dFdx(reference), zy = dFdy(reference);
    float determinant = dx.x * dy.y - dx.y * dy.x;
    vec2 gradient = abs(determinant) > 1e-10 ? vec2(dy.y * zx - dx.y * zy, dx.x * zy - dy.x * zx) / determinant : vec2(0.0);
    gradient = clamp(gradient, vec2(-8.0), vec2(8.0));
    float texel = max(uShadowBounds.z, uShadowBounds.w) / float(textureSize(uShadowMap, 0).x);
    float bias = (0.035 + 0.65 * texel) / uShadowHeight;
    vec2 radius = max(vec2(uShadowSoftness) / uShadowBounds.zw, 0.75 / vec2(textureSize(uShadowMap, 0)));
    float visible = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * radius;
            visible += texture(uShadowMap, vec3(uv + offset, reference + (receiverPlane ? dot(gradient, offset) : 0.0) - bias));
        }
    }
    return visible / 9.0;
}

float shadowVisibility(vec2 position, float height) {
    return shadowVisibility(position, height, true);
}
)";

void bind_shadows(const Shader& shader, const ShadowView* view) {
    glUniform1i(shader.uniform("uShadows"), view != nullptr);
    glUniform1i(shader.uniform("uShadowMap"), 6);
    const Vec2 light = view ? view->light : ShadowView{}.light;
    glUniform2f(shader.uniform("uShadowLight"), light.x, light.y);
    if (!view) return;
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, view->texture);
    glUniform4f(shader.uniform("uShadowBounds"), view->origin.x, view->origin.y, view->size.x, view->size.y);
    glUniform1f(shader.uniform("uShadowHeight"), view->height);
    glUniform1f(shader.uniform("uShadowSoftness"), view->softness);
}
}  // namespace zl
