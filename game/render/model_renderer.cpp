#include "render/model_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "core/base.h"
#include "core/clock.h"
#include "gfx/gl.h"

namespace zl {

namespace {

// The tools are authored about 1.9 m long with the handle along +Y and the
// grip near y=-0.55. The renderer moves that grip to the origin and scales the
// tool to a believable hand-tool length before upload.
constexpr f32 kToolGripY = -0.55f;
constexpr f32 kToolScale = 0.34f;
constexpr f32 kContactRadius = 0.55f;
constexpr f32 kMarkerRadius = 0.62f;
constexpr f32 kMarkerPixels = 13.0f;

static_assert(sizeof(GltfVertex) == 56, "GltfVertex layout is the model vertex format");

const char* kModelVertex = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aJoints;
layout(location = 4) in vec4 aWeights;
uniform mat4 uBones[32];
uniform mat4 uModel;
uniform bool uSkinned;
uniform vec4 uView;       // xy: clip scale, zw: clip offset
uniform mat2 uRotation;   // camera quarter turn
uniform float uDepthScale;
out vec3 vWorld;
out vec3 vNormal;
out vec2 vUV;
void main() {
    vec4 p = vec4(aPosition, 1.0);
    vec3 n = aNormal;
    if (uSkinned) {
        mat4 skin = uBones[int(aJoints.x + 0.5)] * aWeights.x
                  + uBones[int(aJoints.y + 0.5)] * aWeights.y
                  + uBones[int(aJoints.z + 0.5)] * aWeights.z
                  + uBones[int(aJoints.w + 0.5)] * aWeights.w;
        p = skin * p;
        n = mat3(skin) * n;
    }
    vec4 world = uModel * p;
    vWorld = world.xyz;
    vNormal = mat3(uModel) * n;
    // glTF UVs start at the image's top-left; the engine uploads rows top to
    // bottom, so flip V once here.
    vUV = vec2(aUV.x, 1.0 - aUV.y);
    vec2 r = uRotation * world.xy;
    vec2 plane = vec2((r.x - r.y) * 0.7071067812, (r.x + r.y) * 0.3535533906 - world.z * 0.8660254038);
    float depth = -((r.x + r.y) * 0.6123724357 + world.z * 0.5) * uDepthScale;
    gl_Position = vec4(plane * uView.xy + uView.zw, depth, 1.0);
}
)";

const char* kModelFragment = R"(
uniform sampler2D uBaseColor;
uniform vec4 uBaseColorFactor;
uniform float uWaterline;   // world z, tiles
uniform vec3 uViewDir;      // towards the viewer, world space
uniform vec3 uSun;          // towards the sun, world space
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUV;
out vec4 oColor;

void main() {
    if (vWorld.z < uWaterline) discard;
    vec4 texel = texture(uBaseColor, vUV) * uBaseColorFactor;
    if (texel.a < 0.02) discard;
    // The painted world is deliberately flat; lift the darkest texels and
    // keep the painted palette's warm light. The coat is nearly black in the
    // source texture, so a stronger curve keeps its folds readable.
    vec3 albedo = pow(max(texel.rgb, vec3(0.0)), vec3(0.62));
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uSun);
    vec3 V = normalize(uViewDir);
    float ndl = max(dot(N, L), 0.0);
    float fill = max(dot(N, normalize(vec3(V.x, V.y, 0.2))), 0.0);
    float visibility = shadowVisibility(vWorld.xy, vWorld.z, true);
    vec3 ambient = vec3(0.58, 0.55, 0.50);
    vec3 sun = vec3(1.08, 0.98, 0.82) * 0.85;
    vec3 color = albedo * (ambient + sun * ndl * visibility + vec3(0.55, 0.52, 0.46) * fill * 0.38);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 28.0) * 0.25 * visibility;
    color += vec3(0.80, 0.72, 0.56) * spec;
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.24;
    color += vec3(0.58, 0.53, 0.43) * rim;
    oColor = vec4(color, 1.0);
}
)";

const char* kShadowVertex = R"(#version 330 core
uniform vec2 uCenter;
uniform float uRadius;
uniform float uHeight;
uniform vec4 uView;
uniform mat2 uRotation;
uniform float uDepthScale;
out vec2 vUV;
void main() {
    const int corners[6] = int[6](0, 1, 2, 2, 1, 3);
    int corner = corners[gl_VertexID % 6];
    vec2 c = vec2(float(corner & 1), float(corner >> 1));
    vec2 world = uCenter + (c * 2.0 - 1.0) * uRadius;
    vec2 r = uRotation * world;
    vec2 plane = vec2((r.x - r.y) * 0.7071067812, (r.x + r.y) * 0.3535533906 - uHeight * 0.8660254038);
    float depth = -((r.x + r.y) * 0.6123724357 + uHeight * 0.5) * uDepthScale;
    gl_Position = vec4(plane * uView.xy + uView.zw, depth - 0.0005, 1.0);
    vUV = c * 2.0 - 1.0;
}
)";

const char* kShadowFragment = R"(#version 330 core
in vec2 vUV;
out vec4 oColor;
void main() {
    float r = length(vUV);
    float alpha = (1.0 - smoothstep(0.12, 1.0, r)) * 0.42;
    oColor = vec4(0.015, 0.014, 0.012, alpha);
}
)";

const char* kMarkerVertex = R"(#version 330 core
uniform vec2 uCenter;    // screen pixels
uniform float uRadius;   // screen pixels
uniform vec2 uViewport;
out vec2 vUV;
void main() {
    const int corners[6] = int[6](0, 1, 2, 2, 1, 3);
    int corner = corners[gl_VertexID % 6];
    vec2 c = vec2(float(corner & 1), float(corner >> 1));
    vec2 screen = uCenter + (c * 2.0 - 1.0) * uRadius;
    vec2 clip = vec2(screen.x / uViewport.x * 2.0 - 1.0, 1.0 - screen.y / uViewport.y * 2.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    vUV = c * 2.0 - 1.0;
}
)";

const char* kMarkerFragment = R"(#version 330 core
in vec2 vUV;
uniform float uAlpha;
out vec4 oColor;
void main() {
    float r = length(vUV);
    float ring = smoothstep(0.70, 0.84, r) * (1.0 - smoothstep(0.92, 1.0, r));
    oColor = vec4(0.86, 0.82, 0.70, ring * uAlpha);
}
)";

Image white_image() { return Image(1, 1, {255, 255, 255, 255}); }

}  // namespace

ModelRenderer::~ModelRenderer() {
    const auto release = [](std::vector<GpuMesh>& meshes) {
        for (GpuMesh& mesh : meshes) {
            if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
            if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
            if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
            mesh = {};
        }
        meshes.clear();
    };
    release(plague_meshes_);
    release(axe_meshes_);
    release(pickaxe_meshes_);
    if (shadow_vao_) glDeleteVertexArrays(1, &shadow_vao_);
    if (marker_vao_) glDeleteVertexArrays(1, &marker_vao_);
}

void ModelRenderer::init(const char* asset_dir) {
    load(asset_dir);

    const std::string fragment = std::string("#version 330 core\n") + kShadowSamplingGLSL + kModelFragment;
    shader_.compile("model", kModelVertex, fragment.c_str());
    shader_.bind();
    glUniform1i(shader_.uniform("uBaseColor"), 0);
    u_view_ = shader_.uniform("uView");
    u_rotation_ = shader_.uniform("uRotation");
    u_depth_scale_ = shader_.uniform("uDepthScale");
    u_model_ = shader_.uniform("uModel");
    u_bones_ = shader_.uniform("uBones");
    u_skinned_ = shader_.uniform("uSkinned");
    u_base_color_factor_ = shader_.uniform("uBaseColorFactor");
    u_waterline_ = shader_.uniform("uWaterline");
    u_view_dir_ = shader_.uniform("uViewDir");
    u_sun_ = shader_.uniform("uSun");

    shadow_shader_.compile("model shadow", kShadowVertex, kShadowFragment);
    marker_shader_.compile("model marker", kMarkerVertex, kMarkerFragment);
    glGenVertexArrays(1, &shadow_vao_);
    glGenVertexArrays(1, &marker_vao_);
}

void ModelRenderer::load(const char* asset_dir) {
    ready_ = false;
    if (!asset_dir || !*asset_dir) return;
    const std::string root(asset_dir);
    std::string error;

    const Stopwatch timer;
    const std::string plague_path = root + "/models/plague_character.glb";
    if (!plague_.load(plague_path.c_str(), error)) {
        ZL_LOG("model: %s: %s", plague_path.c_str(), error.c_str());
        return;
    }
    upload(plague_, plague_meshes_, plague_materials_, false);
    if (plague_meshes_.empty()) {
        ZL_LOG("model: %s has no drawable primitives", plague_path.c_str());
        return;
    }
    ready_ = true;

    const std::string axe_path = root + "/models/axe.glb";
    if (axe_.load(axe_path.c_str(), error)) upload(axe_, axe_meshes_, axe_materials_, true);
    else ZL_LOG("model: %s: %s", axe_path.c_str(), error.c_str());

    const std::string pickaxe_path = root + "/models/pickaxe.glb";
    if (pickaxe_.load(pickaxe_path.c_str(), error)) upload(pickaxe_, pickaxe_meshes_, pickaxe_materials_, true);
    else ZL_LOG("model: %s: %s", pickaxe_path.c_str(), error.c_str());

    ZL_LOG("model: plague doctor %zu verts, axe %zu, pickaxe %zu, loaded in %.1f ms",
           plague_.primitives.empty() ? 0 : plague_.primitives[0].vertices.size(),
           axe_.primitives.empty() ? 0 : axe_.primitives[0].vertices.size(),
           pickaxe_.primitives.empty() ? 0 : pickaxe_.primitives[0].vertices.size(), timer.millis());
}

void ModelRenderer::upload(const GltfModel& model, std::vector<GpuMesh>& meshes, std::vector<GpuMaterial>& materials, bool tool) {
    materials.clear();
    materials.reserve(std::max<size_t>(model.materials.size(), 1));
    if (model.materials.empty()) {
        GpuMaterial material;
        const Image white = white_image();
        material.texture.create(white, TextureFilter::Linear, TextureWrap::Clamp);
        materials.push_back(std::move(material));
    } else {
        for (const GltfMaterial& source : model.materials) {
            GpuMaterial material;
            const Image image = source.base_color.width > 0 && source.base_color.height > 0 ? source.base_color : white_image();
            material.texture.create(image, TextureFilter::Trilinear, TextureWrap::Repeat);
            for (i32 i = 0; i < 4; ++i) material.factor[i] = source.base_color_factor[i];
            materials.push_back(std::move(material));
        }
    }

    meshes.clear();
    meshes.reserve(model.primitives.size());
    for (const GltfPrimitive& primitive : model.primitives) {
        if (primitive.vertices.empty() || primitive.indices.empty()) continue;
        GpuMesh mesh;
        mesh.count = static_cast<u32>(primitive.indices.size());
        mesh.material = primitive.material >= 0 && primitive.material < static_cast<i32>(materials.size()) ? primitive.material : 0;

        std::vector<GltfVertex> vertices = primitive.vertices;
        if (tool) {
            for (GltfVertex& vertex : vertices) vertex.position = (vertex.position - Vec3{0, kToolGripY, 0}) * kToolScale;
        }

        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GltfVertex)), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(primitive.indices.size() * sizeof(u32)), primitive.indices.data(), GL_STATIC_DRAW);
        const GLsizei stride = static_cast<GLsizei>(sizeof(GltfVertex));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(GltfVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(GltfVertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(GltfVertex, uv)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_SHORT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(GltfVertex, joints)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(GltfVertex, weights)));
        glBindVertexArray(0);
        meshes.push_back(mesh);
    }
}

void ModelRenderer::draw(const IsometricCamera& camera, const ShadowView* shadows, f32 depth_scale,
                         const PlagueDraw& item, const PlaguePose& pose) {
    draw_calls_ = 0;
    if (!ready_ || !pose.valid) return;

    const Camera2D plane = camera.plane_camera();
    const Vec2 scale = plane.clip_scale();
    const Vec2 offset = plane.clip_offset();
    const Vec2 rx = camera.rotate({1, 0}), ry = camera.rotate({0, 1});
    const f32 rotation[] = {rx.x, rx.y, ry.x, ry.y};
    const f32 waterline = item.ground_height + item.wade * kTilesPerMetre;

    const Mat4 world = Mat4::translation({item.position.x, item.position.y, item.ground_height}) *
                       Mat4::rotation_z(item.heading + kPi * 0.5f) *
                       Mat4::scale({kTilesPerMetre, kTilesPerMetre, kTilesPerMetre}) *
                       Mat4::rotation_x(kPi * 0.5f);

    draw_contact_shadow(camera, depth_scale, item);

    shader_.bind();
    glUniform4f(u_view_, scale.x, scale.y, offset.x, offset.y);
    glUniformMatrix2fv(u_rotation_, 1, GL_FALSE, rotation);
    glUniform1f(u_depth_scale_, depth_scale);
    glUniform1f(u_waterline_, waterline);
    // The camera looks along the isometric depth axis; the vector towards the
    // viewer is the same axis because larger depth values are nearer.
    const Vec2 depth_xy = camera.unrotate({IsometricCamera::kDepthXY, IsometricCamera::kDepthXY});
    const Vec3 view_dir = normalize(Vec3{depth_xy.x, depth_xy.y, 0.5f});
    glUniform3f(u_view_dir_, view_dir.x, view_dir.y, view_dir.z);
    const Vec2 light = shadows ? shadows->light : ShadowView{}.light;
    const Vec3 sun = normalize(Vec3{light.x, light.y, 1.0f});
    glUniform3f(u_sun_, sun.x, sun.y, sun.z);
    bind_shadows(shader_, shadows);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    for (const GpuMesh& mesh : plague_meshes_) {
        const GpuMaterial& material = plague_materials_[static_cast<size_t>(mesh.material >= 0 ? mesh.material : 0)];
        draw_mesh(mesh, material, world, pose.bones.data(), static_cast<u32>(pose.bones.size()), true, waterline);
    }

    if (item.show_tool && item.tool != HumanTool::None) {
        const std::vector<GpuMesh>& meshes = item.tool == HumanTool::Axe ? axe_meshes_ : pickaxe_meshes_;
        const std::vector<GpuMaterial>& materials = item.tool == HumanTool::Axe ? axe_materials_ : pickaxe_materials_;
        if (!meshes.empty()) {
            const Mat4 attach = Mat4::rotation_x(-kPi * 0.5f) * Mat4::rotation_y(item.tool == HumanTool::Axe ? -kPi * 0.5f : 0.0f);
            const Mat4 tool_model = world * pose.hand * attach;
            for (const GpuMesh& mesh : meshes) {
                const GpuMaterial& material = materials[static_cast<size_t>(mesh.material >= 0 ? mesh.material : 0)];
                draw_mesh(mesh, material, tool_model, nullptr, 0, false, waterline);
            }
        }
    }
    glDisable(GL_DEPTH_TEST);
}

void ModelRenderer::draw_mesh(const GpuMesh& mesh, const GpuMaterial& material, const Mat4& model_matrix,
                              const Mat4* bones, u32 bone_count, bool skinned, f32 waterline) {
    glUniformMatrix4fv(u_model_, 1, GL_FALSE, model_matrix.m);
    glUniform1i(u_skinned_, skinned ? 1 : 0);
    glUniform1f(u_waterline_, waterline);
    glUniform4fv(u_base_color_factor_, 1, material.factor);
    if (skinned && bones && bone_count > 0) {
        const GLsizei count = static_cast<GLsizei>(std::min<u32>(bone_count, 32));
        glUniformMatrix4fv(u_bones_, count, GL_FALSE, bones[0].m);
    }
    material.texture.bind(0);
    glBindVertexArray(mesh.vao);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.count), GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    ++draw_calls_;
}

void ModelRenderer::draw_contact_shadow(const IsometricCamera& camera, f32 depth_scale, const PlagueDraw& item) {
    const Camera2D plane = camera.plane_camera();
    const Vec2 scale = plane.clip_scale();
    const Vec2 offset = plane.clip_offset();
    const Vec2 rx = camera.rotate({1, 0}), ry = camera.rotate({0, 1});
    const f32 rotation[] = {rx.x, rx.y, ry.x, ry.y};
    const f32 height = item.ground_height + item.wade * kTilesPerMetre;

    shadow_shader_.bind();
    glUniform2f(shadow_shader_.uniform("uCenter"), item.position.x, item.position.y);
    glUniform1f(shadow_shader_.uniform("uRadius"), kContactRadius);
    glUniform1f(shadow_shader_.uniform("uHeight"), height);
    glUniform4f(shadow_shader_.uniform("uView"), scale.x, scale.y, offset.x, offset.y);
    glUniformMatrix2fv(shadow_shader_.uniform("uRotation"), 1, GL_FALSE, rotation);
    glUniform1f(shadow_shader_.uniform("uDepthScale"), depth_scale);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(shadow_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    ++draw_calls_;
}

void ModelRenderer::draw_marker(const IsometricCamera& camera, const PlagueDraw& item) {
    if (!ready_ || item.marker <= 0.0f) return;
    const Vec2 screen = camera.world_to_screen(item.position, item.ground_height + 0.02f);
    const f32 radius = std::max(kMarkerRadius * camera.zoom, kMarkerPixels);
    marker_shader_.bind();
    glUniform2f(marker_shader_.uniform("uCenter"), screen.x, screen.y);
    glUniform1f(marker_shader_.uniform("uRadius"), radius);
    glUniform2f(marker_shader_.uniform("uViewport"), camera.viewport.x, camera.viewport.y);
    glUniform1f(marker_shader_.uniform("uAlpha"), item.marker);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(marker_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    ++draw_calls_;
}

bool ModelRenderer::reveal_bounds(const IsometricCamera& camera, const PlagueDraw& item, Vec2& lo, Vec2& hi) const {
    if (!ready_) return false;
    lo = {1.0e9f, 1.0e9f};
    hi = {-1.0e9f, -1.0e9f};
    constexpr f32 radius = 0.9f;
    for (i32 i = 0; i < 8; ++i) {
        const f32 x = item.position.x + ((i & 1) ? radius : -radius);
        const f32 y = item.position.y + ((i & 2) ? radius : -radius);
        const f32 z = item.ground_height + ((i & 4) ? 2.0f : 0.0f);
        const Vec2 p = camera.project({x, y}, z);
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
    }
    return true;
}

}  // namespace zl
