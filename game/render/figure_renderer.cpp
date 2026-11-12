#include "render/figure_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

#include "gfx/gl.h"
#include "gfx/lighting.h"

namespace zl {

namespace {

// Each part of a figure is four texels in a row of a float texture:
//   0: a.xy, b.xy       screen position of the two ends, tiles from the ground point
//   1: ra, rb, depth of a, depth of b (towards the viewer)
//   2: height of a, height of b, roundness, material + 8 * flags
//   3: colour, group
//   4: projected cutting-edge half-width, world height, view depth
constexpr i32 kTexelsPerPart = 5;
constexpr i32 kRowTexels = kMaxFigureParts * kTexelsPerPart;

// The shader names the materials by number.
static_assert(static_cast<int>(FigureMaterial::Cloth) == 0 && static_cast<int>(FigureMaterial::Skin) == 1 &&
              static_cast<int>(FigureMaterial::Leather) == 2 && static_cast<int>(FigureMaterial::Hair) == 3 &&
              static_cast<int>(FigureMaterial::Metal) == 4 && static_cast<int>(FigureMaterial::Count) <= 8,
              "update the material constants in the figure shader");
static_assert(FigurePartFlag::FlatCaps == 1 && FigurePartFlag::Unshadowed == 2, "update the flag tests in the figure shader");

// A point at height h throws its shadow this far across the ground, in tiles
// per tile of height. The direction is the terrain shader's light; the length
// is cut to about half so the shadow stays under the figure.
constexpr f32 kShadowReach = 0.55f;
constexpr Vec2 kShadowOnGround{0.50f / 0.60f * kShadowReach, 0.62f / 0.60f * kShadowReach};
// The same, as an offset on screen from the point that casts it.
constexpr Vec2 kShadowOnScreen{kShadowOnGround.x, kFigureUpScale + kShadowOnGround.y * kFigureDepthScale};

// Ground effects reach this far from the figure, in tiles.
constexpr f32 kRippleRadius = 0.80f;
constexpr f32 kMarkerRadius = 0.62f;
constexpr f32 kMarkerPixels = 13.0f;
// A live sprite has a finite authoring resolution, like the scenery, rather
// than revealing ever-smaller geometric details as the camera zooms in.
constexpr f32 kPaintedPixelsPerTile = 64.0f;

struct FigureInstance {
    f32 x, y, w, h;                   // quad, in tiles
    f32 ox, oy, parts, waterline;     // ground point; number of parts; waterline in tiles
    f32 row, time, marker, unused;
};

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec4 aRect;
layout(location = 1) in vec4 aFigure;
layout(location = 2) in vec4 aExtra;
uniform vec4 uView;  // xy: clip scale, zw: clip offset
out vec2 vLocal;     // position relative to the figure's ground point, in tiles
flat out vec4 vInfo; // part count, waterline, time, marker
flat out int vRow;
flat out vec3 vAnchor;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 world = aRect.xy + aRect.zw * corner;
    vLocal = world - aFigure.xy;
    vInfo = vec4(aFigure.zw, aExtra.yz);
    vRow = int(aExtra.x + 0.5);
    vAnchor = vec3(aFigure.xy, aExtra.w);
    gl_Position = vec4(world * uView.xy + uView.zw, 0.0, 1.0);
}
)";

const char* kFragment = R"(
uniform sampler2D uParts;
uniform sampler2D uToolArtwork;
uniform bool uHasToolArtwork;
uniform float uPixel;     // tiles per screen pixel
uniform vec2 uShadow;     // screen offset of a shadow per tile of height
uniform vec2 uProjection; // depth and up scales, shared with the CPU projection
uniform vec3 uGround;     // ripple radius, marker radius, marker radius in pixels
uniform bool uPainted;
uniform bool uIsometric;
uniform float uSceneDepthScale;
uniform mat2 uInverseRotation;
in vec2 vLocal;
flat in vec4 vInfo;
flat in int vRow;
flat in vec3 vAnchor;
out vec4 oColor;

// The light and the palette of the painted art (docs/art-direction.md).
uniform vec3 LIGHT;
const vec3 SOOT = vec3(0.102, 0.086, 0.063);
const vec3 BONE = vec3(0.847, 0.812, 0.706);
const vec3 HAZE = vec3(0.56, 0.53, 0.47);
const int CLOTH = 0, SKIN = 1, LEATHER = 2, HAIR = 3, METAL = 4;

float hash(vec2 p) {
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Conservative screen-space bounds avoid the cone's square roots for parts
// that cannot touch this pixel. Keep the same guard for antialiasing/shadows.
bool nearPart(vec2 p, vec2 a, vec2 b, float ra, float rb, float guard) {
    vec2 halfSize = abs(b - a) * 0.5 + vec2(max(ra, rb) + guard);
    return all(lessThanEqual(abs(p - (a + b) * 0.5), halfSize));
}

// Height at the visible surface, not just at the cone's centreline. The
// inverse of the figure projection keeps the water cut level across a boot.
float surfaceHeight(vec4 height, float t, vec2 off, float bulge) {
    return mix(height.x, height.y, t) - off.y * uProjection.y + bulge * uProjection.x;
}

// Signed distance from p to a rounded cone. A rounded cone is the union of
// circles strung along a..b; `t` says which of them p is nearest to, `off` is
// the vector from its centre to p and `r` its radius.
float cone(vec2 p, vec2 a, vec2 b, float ra, float rb, bool flatCaps, out float t, out vec2 off, out float r) {
    vec2 ba = b - a, pa = p - a;
    float h = dot(ba, ba), dr = ra - rb;
    float along = h > 1.0e-10 ? dot(pa, ba) / h : 0.0;
    if (flatCaps) {
        t = clamp(along, 0.0, 1.0);
    } else if (h <= dr * dr + 1.0e-10) {
        t = dr > 0.0 ? 0.0 : 1.0;  // one end swallows the other
    } else {
        float across = abs(pa.x * ba.y - pa.y * ba.x) / h;
        t = clamp(along - across * dr / sqrt(h - dr * dr), 0.0, 1.0);
    }
    r = mix(ra, rb, t);
    off = p - (a + ba * t);
    float d = length(off) - r;
    if (flatCaps) d = max(d, max(-along, along - 1.0) * sqrt(h));
    return d;
}

// Invert the projected 3D shaft/blade plane. A front/back view may mirror
// the painted face; the cutting edge still points along the strike plane.
vec2 toolCoordinates(vec2 p,vec4 ends,vec4 side) {
    vec2 line=ends.zw-ends.xy;
    float len=max(length(line),1e-5);
    vec2 axis=length(line)>1e-5?line/len:vec2(0,-1);
    vec2 normal=vec2(-axis.y,axis.x);
    vec2 across=side.xy;
    float width=dot(across,normal);
    float minimum=0.008;
    if(abs(width)<minimum)across+=normal*((width<0.0?-minimum:minimum)-width);
    float determinant=line.x*across.y-line.y*across.x;
    if(abs(determinant)<1e-8)return vec2(-2.0);
    vec2 q=p-ends.xy;
    return vec2((q.x*across.y-q.y*across.x)/determinant,(line.x*q.y-line.y*q.x)/determinant);
}

vec4 toolSample(vec2 p,vec4 ends,vec4 side,int material) {
    vec2 uv=toolCoordinates(p,ends,side);
    if(uv.x<0.0||uv.x>1.0||abs(uv.y)>1.0)return vec4(0.0);
    return texture(uToolArtwork,vec2((0.5+uv.y*0.5+float(material-5))*0.5,1.0-uv.x));
}

float partDistance(vec2 p,vec4 ends,vec4 size,vec4 side,int code,out float t,out vec2 off,out float r) {
    int material=code&7;
    if(material>=5&&uHasToolArtwork) {
        vec2 uv=toolCoordinates(p,ends,side);
        t=clamp(uv.x,0.0,1.0);off=vec2(uv.y,0.0);r=0.0;
        float alpha=toolSample(p,ends,side,material).a;
        return alpha<0.01?1.0:(0.5-alpha)*uPixel;
    }
    return cone(p,ends.xy,ends.zw,size.x,size.y,(code&8)!=0,t,off,r);
}

float partHeight(vec4 more,vec4 side,float t,vec2 off,float bulge) {
    if((int(more.w+0.5)&7)>=5)return mix(more.x,more.y,t)+off.x*side.z;
    return surfaceHeight(more,t,off,bulge);
}

float partDepth(vec4 size,vec4 more,vec4 side,float t,vec2 off,float bulge) {
    return mix(size.z,size.w,t)+((int(more.w+0.5)&7)>=5?off.x*side.w:bulge);
}

// How much of the light another part keeps off the surface point (p, z):
// two steps along the ray towards the light, asking whether a part stands there.
float occlusion(vec2 p, float z, int count, float water) {
    vec2 p1 = p + LIGHT.xy * 0.030, p2 = p + LIGHT.xy * 0.075;
    float z1 = z + LIGHT.z * 0.030, z2 = z + LIGHT.z * 0.075;
    float dark = 0.0;
    for (int i = 0; i < count; ++i) {
        vec4 ends = texelFetch(uParts, ivec2(i * 5, vRow), 0);
        vec4 size = texelFetch(uParts, ivec2(i * 5 + 1, vRow), 0);
        bool near1 = nearPart(p1, ends.xy, ends.zw, size.x, size.y, 0.0);
        bool near2 = nearPart(p2, ends.xy, ends.zw, size.x, size.y, 0.0);
        if (!near1 && !near2) continue;
        vec4 more = texelFetch(uParts, ivec2(i * 5 + 2, vRow), 0);
        vec4 side=texelFetch(uParts,ivec2(i * 5 + 4,vRow),0);
        int code=int(more.w+0.5);
        float t, r;
        vec2 off;
        if (near1 && partDistance(p1,ends,size,side,code,t,off,r) < 0.0 && partHeight(more,side,t,off,sqrt(max(r * r - dot(off, off),0.0))) >= water) {
            float bulge = sqrt(max(r * r - dot(off, off), 0.0));
            float middle=partDepth(size,more,side,t,off,0.0);
            dark = max(dark, smoothstep(0.004, 0.020, middle + bulge - z1) * step(middle - bulge, z1 + 0.12));
        }
        if (near2 && partDistance(p2,ends,size,side,code,t,off,r) < 0.0 && partHeight(more,side,t,off,sqrt(max(r * r - dot(off, off),0.0))) >= water) {
            float bulge = sqrt(max(r * r - dot(off, off), 0.0));
            float middle=partDepth(size,more,side,t,off,0.0);
            dark = max(dark, smoothstep(0.004, 0.020, middle + bulge - z2) * step(middle - bulge, z2 + 0.12));
        }
    }
    return dark;
}

// The colour of part i at p: its own colour in shadow, half tone, light and a
// sparing highlight, the steps broken up like brush strokes laid along the form.
// Also returns its distance, its group, and `rim`: 1 where its edge faces away
// from the light.
vec3 paint(int i, vec2 p, int count, float water, bool shadowed, out float d, out float group, out float rim) {
    vec4 ends = texelFetch(uParts, ivec2(i * 5, vRow), 0);
    vec4 size = texelFetch(uParts, ivec2(i * 5 + 1, vRow), 0);
    vec4 more = texelFetch(uParts, ivec2(i * 5 + 2, vRow), 0);
    vec4 tint = texelFetch(uParts, ivec2(i * 5 + 3, vRow), 0);
    vec4 side=texelFetch(uParts,ivec2(i * 5 + 4,vRow),0);
    int code = int(more.w + 0.5);
    int material = code & 7;
    float t, r;
    vec2 off;
    d = partDistance(p,ends,size,side,code,t,off,r);
    group = tint.a;
    if(material>=5&&uHasToolArtwork) {
        vec4 artwork=toolSample(p,ends,side,material);
        rim=0.0;
        return artwork.rgb/max(artwork.a,1e-5);
    }

    vec2 slope = off / max(r, 1.0e-5) * more.z;
    if (uPainted) slope *= 0.72; // flatter painted forms, less cylindrical lighting
    float steep = dot(slope, slope);
    if (steep > 0.97) slope *= sqrt(0.97 / steep);
    vec3 n = vec3(slope, sqrt(1.0 - dot(slope, slope)));
    float light = dot(n, LIGHT);

    // Brush marks: noise stretched along the part, in the part's own space so
    // that it moves with the limb.
    vec2 ba = ends.zw - ends.xy;
    float len = length(ba);
    vec2 axis = len > 1.0e-5 ? ba / len : vec2(0.0, 1.0);
    vec2 q = vec2(dot(p - ends.xy, axis), dot(p - ends.xy, vec2(-axis.y, axis.x)));
    float seed = float(i) * 7.13;
    float dab = noise(q * vec2(24.0, 58.0) + seed) - 0.5;
    float grain = material == CLOTH ? 0.30 : (material == LEATHER ? 0.20 : 0.10);
    if (material == HAIR) {
        dab = noise(q * (uPainted ? vec2(10.0, 85.0) : vec2(8.0, 170.0)) + seed) - 0.5;
        grain = uPainted ? 0.32 : 0.62;
    }
    light += dab * grain;
    if (shadowed && (code & 16) == 0) {
        float z = mix(size.z, size.w, t) + sqrt(max(r * r - dot(off, off), 0.0));
        light = mix(light, min(light, -0.25), occlusion(p, z, count, water) * (uPainted ? 0.62 : 1.0));
    }

    vec3 base = tint.rgb;
    vec3 dark = mix(base * 0.50, SOOT, 0.34);
    vec3 half_tone = base * 0.77;
    vec3 high = mix(base, BONE, 0.40) * 1.08;
    float gloss = 0.22;
    if (material == SKIN) {
        dark = base * vec3(0.60, 0.45, 0.41);
        half_tone = base * vec3(0.86, 0.77, 0.73);
        gloss = 0.50;
    } else if (material == LEATHER) {
        dark = mix(base * 0.40, SOOT, 0.42);
        gloss = 0.55;
    } else if (material == HAIR) {
        high = base * 1.55 + 0.04;
        gloss = 0.65;
    } else if (material == METAL) {
        dark = mix(base * 0.34, SOOT, 0.3);
        high = BONE;
        gloss = 1.0;
    }
    // Broader transitions and quieter highlights read as pigment rather than
    // separate glossy tubes, while retaining the material colours.
    if (uPainted) gloss *= 0.55;
    float W = uPainted ? 0.10 : 0.04;
    vec3 c = mix(dark, half_tone, smoothstep(0.02 - W, 0.02 + W, light));
    c = mix(c, base, smoothstep(0.40 - W, 0.40 + W, light));
    c = mix(c, high, gloss * smoothstep(0.84 - W, 0.84 + W, light));

    vec2 outward = off / max(length(off), 1.0e-6);
    rim = 1.0 - smoothstep(-0.35, 0.10, dot(outward, normalize(LIGHT.xy)));
    return c;
}

vec4 sceneShadow(vec4 color, float depth) {
    if (!uIsometric || !uShadows) return color;
    vec2 plane = vLocal + vAnchor.xy;
    float height = (-depth / uSceneDepthScale - plane.y * 1.732050808) * 0.5;
    float sum = (plane.y + height * 0.866025404) / 0.353553391;
    float difference = plane.x / 0.707106781;
    vec2 position = uInverseRotation * vec2(sum + difference, sum - difference) * 0.5;
    if(color.a>0.0001)color.rgb=scenePaintedShadow(color.rgb/color.a,shadowVisibility(position,height,false))*color.a;
    return color;
}

void main() {
    float sceneDepth = uIsometric ? vAnchor.z - vLocal.y * 1.732050808 * uSceneDepthScale : 0.0;
    gl_FragDepth = sceneDepth * 0.5 + 0.5;
    int count = int(vInfo.x + 0.5);
    float water = vInfo.y;
    vec2 p = vLocal;
    float aa = uPixel;

    // Pass over every part: which two are nearest the viewer here, how far the
    // pixel is inside the figure as a whole, and how much shadow the figure
    // throws on the ground at this point.
    int first = -1, second = -1;
    float zFirst = -1.0e9, zSecond = -1.0e9;
    float inside = 1.0e9, shadow = 0.0;
    for (int i = 0; i < count; ++i) {
        vec4 ends = texelFetch(uParts, ivec2(i * 5, vRow), 0);
        vec4 size = texelFetch(uParts, ivec2(i * 5 + 1, vRow), 0);
        vec4 more = texelFetch(uParts, ivec2(i * 5 + 2, vRow), 0);
        vec4 side=texelFetch(uParts,ivec2(i * 5 + 4,vRow),0);
        float t, r;
        vec2 off;
        if (nearPart(p, ends.xy, ends.zw, size.x, size.y, 0.5 * aa)) {
            float d = partDistance(p,ends,size,side,int(more.w+0.5),t,off,r);
            float bulge = sqrt(max(r * r - dot(off, off), 0.0));
            if (water > 0.0) d = max(d, water - partHeight(more,side,t,off,bulge));
            if (d < 0.5 * aa) {
                inside = min(inside, d);
                float z=partDepth(size,more,side,t,off,bulge);
                if (z > zFirst) {
                    second = first;
                    zSecond = zFirst;
                    first = i;
                    zFirst = z;
                } else if (z > zSecond) {
                    second = i;
                    zSecond = z;
                }
            }
        }
        if (max(more.x, more.y) > water) {
            float ha = max(more.x - water, 0.0), hb = max(more.y - water, 0.0);
            vec2 a = ends.xy + ha * uShadow, b = ends.zw + hb * uShadow;
            if (!nearPart(p, a, b, size.x * 0.85, size.y * 0.85, 0.012 + 0.075 * max(ha, hb))) continue;
            int shadowCode=int(more.w+0.5);
            if((shadowCode&7)<5)shadowCode=shadowCode&(~8);
            vec4 shadowSide=side;shadowSide.xy=(side.xy+uShadow*side.z)*0.85;
            float d2 = partDistance(p,vec4(a,b),vec4(size.xy*0.85,size.zw),shadowSide,shadowCode,t,off,r);
            // The further the shadow falls, the softer its edge.
            float soft = 0.012 + 0.075 * mix(ha, hb, t);
            shadow = max(shadow, 1.0 - smoothstep(-soft, soft, d2));
        }
    }

    // --- the ground under the figure: its shadow, ripples if it is wading,
    // and a ring to find it by when the camera is far away
    vec2 ground = vec2(p.x, p.y / uProjection.x);
    float reach = length(ground);
    vec3 under = SOOT;
    float underAlpha = shadow * 0.44;
    if (water > 0.0) {
        float ring = smoothstep(0.50, 0.95, sin(reach * 30.0 - vInfo.z * 4.5));
        float foam = ring * (1.0 - smoothstep(0.22, uGround.x, reach)) * smoothstep(0.0, 0.08, water) * 0.34;
        under = mix(under * underAlpha, BONE, foam) / max(mix(underAlpha, 1.0, foam), 1.0e-4);
        underAlpha = mix(underAlpha, 1.0, foam);
    }
    if (vInfo.w > 0.0) {
        float radius = max(uGround.y, uGround.z * aa);
        float width = max(0.035, 1.2 * aa);
        float ring = (1.0 - smoothstep(width, width + aa, abs(reach - radius))) * vInfo.w * 0.85;
        under = mix(under * underAlpha, BONE, ring) / max(mix(underAlpha, 1.0, ring), 1.0e-4);
        underAlpha = mix(underAlpha, 1.0, ring);
    }

    if (first < 0) {
        oColor = sceneShadow(vec4(under * underAlpha, underAlpha), sceneDepth);
        return;
    }

    // --- the figure itself
    float d1, g1, rim1;
    vec3 c1 = paint(first, p, count, water, true, d1, g1, rim1);
    float a1 = clamp(0.5 - d1 / aa, 0.0, 1.0);
    // A dark edge, not an outline: only where the nearest part ends on its
    // shadow side against something that is not the same surface.
    float edgeWidth = max(0.012, 0.8 * aa);
    float edge = smoothstep(-edgeWidth, -0.35 * edgeWidth, d1) * rim1 * 0.86;
    if (uPainted) edge *= 0.60;
    vec3 color = c1;
    if (second >= 0) {
        float d2, g2, rim2;
        vec3 c2 = paint(second, p, count, water, false, d2, g2, rim2);
        float gap = zFirst - zSecond;
        bool same = abs(g1 - g2) < 0.5;
        // Parts of one surface melt into each other; different surfaces meet
        // along a line one pixel wide.
        float band = same ? max(1.5 * aa, 0.014) : aa;
        if (same) edge *= smoothstep(0.02, 0.05, gap);
        color = mix(c2, mix(c1, SOOT, edge), a1 * (0.5 + 0.5 * smoothstep(0.0, band, gap)));
    } else {
        color = mix(c1, SOOT, edge);
    }

    // The same overcast, smoke-stained air as the terrain, laid on more thinly
    // so that the figure stands out from the ground.
    color = mix(vec3(dot(color, vec3(0.299, 0.587, 0.114))), color, 0.94);
    color = mix(color, HAZE, 0.035);

    float alpha = clamp(0.5 - inside / aa, 0.0, 1.0);
    if (uIsometric) {
        sceneDepth = vAnchor.z - (zFirst - water * uProjection.x) * uSceneDepthScale;
        gl_FragDepth = sceneDepth * 0.5 + 0.5;
    }
    oColor = sceneShadow(vec4(color * alpha + under * underAlpha * (1.0 - alpha), alpha + underAlpha * (1.0 - alpha)), sceneDepth);
}
)";

}  // namespace

FigureRenderer::~FigureRenderer() {
    if (parts_) glDeleteTextures(1, &parts_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void FigureRenderer::init(const char* asset_dir) {
    const std::string fragment = std::string("#version 330 core\n") + kShadowSamplingGLSL + kSceneLightingGLSL + kFragment;
    shader_.compile("figure", kVertex, fragment.c_str());
    shader_.bind();
    glUniform1i(shader_.uniform("uParts"), 0);
    glUniform2f(shader_.uniform("uShadow"), kShadowOnScreen.x, kShadowOnScreen.y);
    glUniform2f(shader_.uniform("uProjection"), kFigureDepthScale, kFigureUpScale);
    glUniform3f(shader_.uniform("uGround"), kRippleRadius, kMarkerRadius, kMarkerPixels);
    u_view_ = shader_.uniform("uView");
    u_pixel_ = shader_.uniform("uPixel");
    u_painted_ = shader_.uniform("uPainted");
    sprites_.init();
    reload_art(asset_dir);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size_);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const GLsizei stride = sizeof(FigureInstance);
    for (GLuint attribute = 0; attribute < 3; ++attribute) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribPointer(attribute, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(static_cast<size_t>(attribute) * 4 * sizeof(f32)));
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);

    glGenTextures(1, &parts_);
    glBindTexture(GL_TEXTURE_2D, parts_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

void FigureRenderer::reload_art(const char* asset_dir) {
#ifdef ZL_ASSET_DIR
    if(!asset_dir)asset_dir=ZL_ASSET_DIR;
#endif
    Image atlas(256,128);
    have_tool_art_=true;
    const char* names[]={"axe","pickaxe"};
    for(int i=0;i<2;++i) {
        Image source;
        const std::string path=std::string(asset_dir?asset_dir:"")+"/tools/"+names[i]+".png";
        if(!read_png(path.c_str(),source)||source.width!=128||source.height!=128) {
            ZL_LOG("art: missing or invalid held-tool artwork %s",path.c_str());have_tool_art_=false;continue;
        }
        for(int y=0;y<128;++y)for(int x=0;x<128;++x) {
            Color c=source.at(x,y);const u32 a=c.a;
            c.r=static_cast<u8>((c.r*a+127)/255);c.g=static_cast<u8>((c.g*a+127)/255);c.b=static_cast<u8>((c.b*a+127)/255);
            atlas.at(i*128+x,y)=c;
        }
    }
    tool_art_.create(atlas,TextureFilter::Linear,TextureWrap::Clamp);
}

void FigureRenderer::draw(const FigureDraw* figures, u32 count, const Camera2D& camera,
                          const IsometricCamera* world_camera, const ShadowView* shadows, f32 depth_scale) {
    draw_calls_ = 0;
    if (count == 0) return;
    const bool painted = style_ == FigureStyle::Painted;
    const f32 raster_zoom = painted ? std::min(camera.zoom, kPaintedPixelsPerTile) : camera.zoom;
    const f32 pixel = 1.0f / raster_zoom;
    const Vec2 view_lo = camera.visible_min(), view_hi = camera.visible_max();
    const f32 up_scale = world_camera ? IsometricCamera::kZ : kFigureUpScale;
    const f32 ground_scale = world_camera ? 0.5f : kFigureDepthScale;
    const Vec2 shadow = world_camera ? world_camera->project(ShadowView{}.light * -1.0f) + Vec2{0, up_scale} : kShadowOnScreen;
    const auto project_part = [&](Vec3 p) {
        if (!world_camera) return p;
        const Vec2 r = world_camera->rotate({p.x, p.y});
        return Vec3{(r.x - r.y) * IsometricCamera::kX, (r.x + r.y) * IsometricCamera::kX, p.z};
    };

    std::vector<FigureInstance> instances;
    instances.reserve(count);
    texels_.clear();
    for (u32 k = 0; k < count; ++k) {
        const FigureDraw& item = figures[k];
        if (!item.figure || item.figure->parts.empty()) continue;
        const Figure& figure = *item.figure;
        const i32 parts = std::min(static_cast<i32>(figure.parts.size()), kMaxFigureParts);
        // A wading figure stands on the bed, so it sits lower on the screen
        // and the water cuts it off where the map's surface is.
        const f32 water = std::max(figure.waterline, 0.0f);
        const f32 sink = water * up_scale;
        const Vec2 standing = world_camera ? world_camera->project(item.position, item.height) : item.position;

        const size_t row_start = texels_.size();
        texels_.resize(row_start + static_cast<size_t>(kRowTexels) * 4, 0.0f);
        f32* out = texels_.data() + row_start;
        Vec2 lo{-0.05f, -0.05f}, hi{0.05f, 0.05f};
        const auto grow = [&](Vec2 centre, f32 radius) {
            lo.x = std::min(lo.x, centre.x - radius);
            lo.y = std::min(lo.y, centre.y - radius);
            hi.x = std::max(hi.x, centre.x + radius);
            hi.y = std::max(hi.y, centre.y + radius);
        };
        for (i32 i = 0; i < parts; ++i) {
            const FigurePart& part = figure.parts[static_cast<size_t>(i)];
            const Vec3 pa = project_part(part.a), pb = project_part(part.b);
            Vec2 a{pa.x, pa.y * ground_scale - pa.z * up_scale};
            Vec2 b{pb.x, pb.y * ground_scale - pb.z * up_scale};
            a.y += sink;
            b.y += sink;
            const Vec3 projected_side=project_part(part.image_side);
            const f32 texel[20] = {
                a.x, a.y, b.x, b.y,
                part.ra, part.rb, pa.y * up_scale + pa.z * ground_scale, pb.y * up_scale + pb.z * ground_scale,
                part.a.z, part.b.z, part.roundness, static_cast<f32>(static_cast<int>(part.material) + 8 * part.flags),
                static_cast<f32>(part.color.r) / 255.0f, static_cast<f32>(part.color.g) / 255.0f, static_cast<f32>(part.color.b) / 255.0f, static_cast<f32>(part.group),
                projected_side.x,projected_side.y*ground_scale-projected_side.z*up_scale,part.image_side.z,projected_side.y*up_scale+projected_side.z*ground_scale,
            };
            std::copy_n(texel,20,out+static_cast<size_t>(i)*20);
            grow(a, part.ra);
            grow(b, part.rb);
            // And the shadow it throws, with room for its soft edge.
            const f32 ha = std::max(part.a.z - water, 0.0f), hb = std::max(part.b.z - water, 0.0f);
            grow(a + shadow * ha, part.ra + 0.02f + 0.08f * ha);
            grow(b + shadow * hb, part.rb + 0.02f + 0.08f * hb);
            if(part.material>=FigureMaterial::ToolAxe) {
                const Vec2 side{projected_side.x,projected_side.y*ground_scale-projected_side.z*up_scale};
                for(float sign:{-1.0f,1.0f}) {
                    const float za=std::max(part.a.z+sign*part.image_side.z-water,0.0f);
                    const float zb=std::max(part.b.z+sign*part.image_side.z-water,0.0f);
                    grow(a+side*sign+shadow*za,0.02f+0.08f*za);
                    grow(b+side*sign+shadow*zb,0.02f+0.08f*zb);
                }
            }
        }
        if (water > 0.0f) {
            grow({0.0f, 0.0f}, kRippleRadius);
        }
        if (item.marker > 0.0f) {
            const f32 radius = std::max(kMarkerRadius, kMarkerPixels * pixel) + std::max(0.035f, 1.2f * pixel);
            grow({0.0f, 0.0f}, radius);
        }
        const f32 margin = 2.0f * pixel;
        lo -= Vec2{margin, margin};
        hi += Vec2{margin, margin};
        if (painted) {
            // Keep the texel grid anchored to the feet, not to pose bounds or
            // camera position, so changing silhouettes do not shift the image.
            lo = {std::floor(lo.x * raster_zoom) * pixel, std::floor(lo.y * raster_zoom) * pixel};
            hi = {std::ceil(hi.x * raster_zoom) * pixel, std::ceil(hi.y * raster_zoom) * pixel};
        }

        const Vec2 world_lo = standing + lo, world_hi = standing + hi;
        if (world_hi.x < view_lo.x || world_hi.y < view_lo.y || world_lo.x > view_hi.x || world_lo.y > view_hi.y) {
            texels_.resize(row_start);  // off screen
            continue;
        }
        FigureInstance instance;
        instance.x = world_lo.x;
        instance.y = world_lo.y;
        instance.w = hi.x - lo.x;
        instance.h = hi.y - lo.y;
        instance.ox = standing.x;
        instance.oy = standing.y;
        instance.parts = static_cast<f32>(parts);
        instance.waterline = water;
        instance.row = static_cast<f32>(instances.size());
        instance.time = item.time;
        instance.marker = item.marker;
        instance.unused = world_camera ? -world_camera->depth(item.position, item.height) * depth_scale : 0.0f;
        instances.push_back(instance);
    }
    if (instances.empty()) return;
    const i32 used_rows = static_cast<i32>(instances.size());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, parts_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (used_rows > rows_) {
        rows_ = std::max(used_rows, rows_ * 2);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kRowTexels, rows_, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kRowTexels, used_rows, GL_RGBA, GL_FLOAT, texels_.data());

    const u32 wanted = static_cast<u32>(instances.size());
    // Render the visible poses together into padded atlas cells. The atlas
    // grows as needed and stays on the GPU; no images are read back or saved.
    i32 cell_width = 0, cell_height = 0;
    for (const FigureInstance& item : instances) {
        cell_width = std::max(cell_width, static_cast<i32>(std::lround(item.w * raster_zoom)) + 2);
        cell_height = std::max(cell_height, static_cast<i32>(std::lround(item.h * raster_zoom)) + 2);
    }
    const i32 columns = std::max(1, static_cast<i32>(std::ceil(std::sqrt(static_cast<f32>(wanted)))));
    const i32 rows = static_cast<i32>((wanted + static_cast<u32>(columns) - 1) / static_cast<u32>(columns));
    const bool use_atlas = painted && cell_width <= max_texture_size_ / columns && cell_height <= max_texture_size_ / rows;
    GLint saved_draw = 0, saved_read = 0, saved_viewport[4]{};
    GLfloat saved_clear[4]{};
    std::vector<FigureInstance> baked;
    if (use_atlas) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read);
        glGetIntegerv(GL_VIEWPORT, saved_viewport);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, saved_clear);
        const i32 width = cell_width * columns, height = cell_height * rows;
        if (width > painted_.width() || height > painted_.height()) {
            const i32 grow_width = std::min(max_texture_size_, std::max(width, painted_.width() + painted_.width() / 2));
            const i32 grow_height = std::min(max_texture_size_, std::max(height, painted_.height() + painted_.height() / 2));
            painted_.create(grow_width, grow_height);
        }
        painted_.bind();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        baked = instances;
        for (u32 i = 0; i < wanted; ++i) {
            FigureInstance& item = baked[i];
            const f32 x = static_cast<f32>((i % static_cast<u32>(columns)) * static_cast<u32>(cell_width) + 1) * pixel;
            const f32 y = static_cast<f32>((i / static_cast<u32>(columns)) * static_cast<u32>(cell_height) + 1) * pixel;
            item.ox += x - item.x;
            item.oy += y - item.y;
            item.x = x;
            item.y = y;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (wanted > instance_capacity_) {
        instance_capacity_ = wanted + wanted / 2;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(instance_capacity_) * static_cast<GLsizeiptr>(sizeof(FigureInstance)), nullptr, GL_STREAM_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(wanted) * static_cast<GLsizeiptr>(sizeof(FigureInstance)), use_atlas ? baked.data() : instances.data());

    shader_.bind();
    glUniform2f(shader_.uniform("uProjection"), ground_scale, up_scale);
    tool_art_.bind(1);
    glUniform1i(shader_.uniform("uToolArtwork"),1);
    glUniform1i(shader_.uniform("uHasToolArtwork"),have_tool_art_);
    glUniform2f(shader_.uniform("uShadow"), shadow.x, shadow.y);
    if (world_camera) {
        const Vec2 light{ShadowView{}.light};
        const Vec2 p = world_camera->project(light, 1.0f);
        const f32 d = world_camera->depth(light, 1.0f);
        const f32 inverse_length = 1.0f / std::sqrt(dot(light, light) + 1.0f);
        glUniform3f(shader_.uniform("LIGHT"), p.x * inverse_length, p.y * inverse_length, d * inverse_length);
        const Vec2 x = world_camera->unrotate({1, 0}), y = world_camera->unrotate({0, 1});
        const f32 inverse[] = {x.x, x.y, y.x, y.y};
        glUniformMatrix2fv(shader_.uniform("uInverseRotation"), 1, GL_FALSE, inverse);
    } else {
        glUniform3f(shader_.uniform("LIGHT"), -0.5013f, -0.6216f, 0.6016f);
    }
    glUniform1i(shader_.uniform("uIsometric"), world_camera && !use_atlas);
    glUniform1f(shader_.uniform("uSceneDepthScale"), depth_scale);
    bind_shadows(shader_, !use_atlas ? shadows : nullptr);
    const GLboolean had_depth = glIsEnabled(GL_DEPTH_TEST);
    if (use_atlas || !world_camera) glDisable(GL_DEPTH_TEST);
    Camera2D raster_camera = camera;
    if (use_atlas) {
        raster_camera.zoom = raster_zoom;
        raster_camera.viewport = {static_cast<f32>(painted_.width()), static_cast<f32>(painted_.height())};
        raster_camera.center = raster_camera.viewport * (0.5f * pixel);
    }
    const Vec2 scale = raster_camera.clip_scale(), offset = raster_camera.clip_offset();
    glUniform4f(u_view_, scale.x, scale.y, offset.x, offset.y);
    glUniform1f(u_pixel_, use_atlas ? pixel : 1.0f / camera.zoom);
    glUniform1i(u_painted_, painted ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, parts_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(wanted));
    glBindVertexArray(0);
    draw_calls_ = 1;

    if (use_atlas) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_draw));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(saved_read));
        glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
        glClearColor(saved_clear[0], saved_clear[1], saved_clear[2], saved_clear[3]);
        SpriteInstance* out = sprites_.begin(wanted);
        const auto uv = [](f32 value) { return static_cast<u16>(clamp(std::lround(value * 65535.0f), 0l, 65535l)); };
        for (u32 i = 0; i < wanted; ++i) {
            const FigureInstance &item = instances[i], &source = baked[i];
            const f32 u0 = source.x * raster_zoom / static_cast<f32>(painted_.width());
            const f32 v0 = 1.0f - source.y * raster_zoom / static_cast<f32>(painted_.height());
            const f32 u1 = (source.x + source.w) * raster_zoom / static_cast<f32>(painted_.width());
            const f32 v1 = 1.0f - (source.y + source.h) * raster_zoom / static_cast<f32>(painted_.height());
            out[i] = {item.x, item.y, item.w, item.h, uv(u0), uv(v0), uv(u1), uv(v1), 0xFFFFFFFFu};
            if (world_camera) {
                out[i].depth = item.unused - kSpriteDepthBias * depth_scale;
                out[i].anchor = (item.oy - item.y) / item.h;
                out[i].depth_span = item.h / (2.0f * IsometricCamera::kZ) * depth_scale;
                out[i].ground_span = -item.h * (IsometricCamera::kDepthXY / IsometricCamera::kY) * depth_scale;
            }
        }
        if (had_depth) glEnable(GL_DEPTH_TEST);
        sprites_.draw(wanted, camera, painted_.color_texture(), shadows, world_camera, depth_scale);
        draw_calls_ = 2;
    }
    if (had_depth) glEnable(GL_DEPTH_TEST);
}

}  // namespace zl
