#include "gfx/shadows.h"

#include "gfx/gl.h"
#include "gfx/shader.h"

namespace zl {

const char* kShadowSamplingGLSL = R"(
uniform sampler2DShadow uShadowMap;
uniform sampler2D uShadowDepth;
uniform bool uShadowDepthAvailable;
uniform vec4 uShadowBounds;
uniform vec2 uShadowLight;
uniform float uShadowHeight;
uniform float uShadowSoftness;
uniform bool uShadows;

bool ignoredShadowBlocker(vec2 uv, float depth, vec3 low, vec3 high) {
    if (high.z <= low.z || !uShadowDepthAvailable) return false;
    vec2 resolution = vec2(textureSize(uShadowDepth, 0));
    vec2 sampleUV = (floor(uv * resolution) + 0.5) / resolution;
    float height = (1.0 - depth) * uShadowHeight;
    vec2 position = uShadowBounds.xy + sampleUV * uShadowBounds.zw + uShadowLight * height;
    vec3 point = vec3(position, height);
    return all(greaterThanEqual(point, low - 0.002)) && all(lessThanEqual(point, high + 0.002));
}

float shadowVisibility(vec2 position, float height, bool receiverPlane, vec3 ignoredLow, vec3 ignoredHigh) {
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
    float bias = (receiverPlane ? (0.008 + 0.25 * texel) : (0.035 + 0.65 * texel)) / uShadowHeight;
    float softness=uShadowSoftness;
    if(uShadowDepthAvailable) {
        // Low props need a firm contact edge; high casters retain a broad
        // penumbra. Search a few nearby blockers instead of blurring every
        // shadow by the same world-space radius.
        float gap=0.0,blockers=0.0;
        vec2 search=vec2(uShadowSoftness)/uShadowBounds.zw;
        const vec2 offsets[5]=vec2[5](vec2(0),vec2(-1,0),vec2(1,0),vec2(0,-1),vec2(0,1));
        for(int i=0;i<5;++i) {
            float depth=texture(uShadowDepth,uv+offsets[i]*search).r;
            if(depth<reference-bias) {gap+=(reference-depth)*uShadowHeight;blockers+=1.0;}
        }
        softness=clamp(0.012+0.09*gap/max(blockers,1.0),0.012,uShadowSoftness);
    }
    vec2 radius = max(vec2(softness) / uShadowBounds.zw, 0.6 / vec2(textureSize(uShadowMap, 0)));
    float visible = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x, y) * radius;
            float sampleReference = reference + (receiverPlane ? dot(gradient, offset) : 0.0) - bias;
            float sampleVisibility;
            if (ignoredHigh.z > ignoredLow.z && uShadowDepthAvailable) {
                // Compare the same nearest depth texel that we classify below;
                // hardware PCF would mix in neighboring proxy texels first.
                float blocker = texture(uShadowDepth, uv + offset).r;
                sampleVisibility = ignoredShadowBlocker(uv + offset, blocker, ignoredLow, ignoredHigh)
                    ? 1.0 : step(sampleReference, blocker);
            } else sampleVisibility = texture(uShadowMap, vec3(uv + offset, sampleReference));
            visible += sampleVisibility;
        }
    }
    return visible / 9.0;
}

float shadowVisibility(vec2 position, float height, bool receiverPlane) {
    return shadowVisibility(position, height, receiverPlane, vec3(0.0), vec3(0.0));
}

float shadowVisibility(vec2 position, float height) {
    return shadowVisibility(position, height, true);
}
)";

void bind_shadows(const Shader& shader, const ShadowView* view) {
    glUniform1i(shader.uniform("uShadows"), view != nullptr);
    glUniform1i(shader.uniform("uShadowMap"), 6);
    glUniform1i(shader.uniform("uShadowDepth"),7);
    glUniform1i(shader.uniform("uShadowDepthAvailable"),view&&view->depth_sampler!=0);
    const Vec2 light = view ? view->light : ShadowView{}.light;
    glUniform2f(shader.uniform("uShadowLight"), light.x, light.y);
    if (!view) return;
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, view->texture);
    glActiveTexture(GL_TEXTURE7);glBindTexture(GL_TEXTURE_2D,view->texture);glBindSampler(7,view->depth_sampler);
    glUniform4f(shader.uniform("uShadowBounds"), view->origin.x, view->origin.y, view->size.x, view->size.y);
    glUniform1f(shader.uniform("uShadowHeight"), view->height);
    glUniform1f(shader.uniform("uShadowSoftness"), view->softness);
}
}  // namespace zl
