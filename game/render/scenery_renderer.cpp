#include "render/scenery_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>

#include "core/rng.h"
#include "gfx/gl.h"
#include "gfx/lighting.h"

namespace zl {
namespace {
struct Vertex { Vec3 position, normal; Vec2 uv; };
const char* kVertex = R"(#version 330 core
layout(location=0) in vec3 aPosition;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aInstance;
layout(location=4) in vec2 aRotation;
layout(location=5) in vec4 aColor;
uniform vec4 uView;
uniform mat2 uRotation;
uniform float uDepthScale;
uniform bool uShadowPass;
uniform vec4 uShadowBounds;
uniform vec2 uShadowLight;
uniform float uShadowHeight;
uniform sampler2D uHeights;
uniform vec2 uMapSize;
uniform float uStep;
out vec3 vWorld;
out vec3 vNormal;
centroid out vec2 vUV;
out vec4 vColor;
out vec2 vPlane;
float groundAt(vec2 p) { return texture(uHeights,p/uMapSize).r; }
float surfaceAt(vec2 p) {
    vec2 lo=floor(p/uStep)*uStep, hi=min(lo+uStep,uMapSize);
    vec2 f=clamp((p-lo)/max(hi-lo,vec2(0.0001)),0.0,1.0);
    float h10=groundAt(vec2(hi.x,lo.y)),h01=groundAt(vec2(lo.x,hi.y));
    if(f.x+f.y<=1.0) {float h=groundAt(lo);return h+(h10-h)*f.x+(h01-h)*f.y;}
    float h=groundAt(hi);return h+(h01-h)*(1.0-f.x)+(h10-h)*(1.0-f.y);
}
void main() {
    mat2 turn=mat2(aRotation.x,aRotation.y,-aRotation.y,aRotation.x);
    vec3 p=vec3(turn*aPosition.xy,aPosition.z)*aInstance.w;
    p.xy+=aInstance.xy;p.z+=surfaceAt(aInstance.xy);
    vWorld=p;vNormal=vec3(turn*aNormal.xy,aNormal.z);vUV=aUV;vColor=aColor;
    vec2 r=uRotation*p.xy;
    vPlane=vec2((r.x-r.y)*0.7071067812,(r.x+r.y)*0.3535533906-p.z*0.8660254038);
    gl_Position=vec4(vPlane*uView.xy+uView.zw,-((r.x+r.y)*0.6123724357+p.z*0.5)*uDepthScale,1);
    if(uShadowPass) {
        vec2 q=(p.xy-uShadowLight*p.z-uShadowBounds.xy)/uShadowBounds.zw;
        gl_Position=vec4(q*2.0-1.0,1.0-2.0*p.z/uShadowHeight,1);
    }
}
)";
const char* kFragment = R"(
uniform sampler2D uBaseColor;
uniform vec4 uFactor;
uniform bool uShadowPass;
uniform vec3 uSun;
uniform vec3 uViewDir;
uniform sampler2D uMetallicRoughness;
uniform vec2 uMaterial; // metallic and roughness factors
uniform bool uReveal;
uniform vec4 uRevealBounds;
uniform float uOpacity;
in vec3 vWorld;
in vec3 vNormal;
centroid in vec2 vUV;
in vec4 vColor;
in vec2 vPlane;
out vec4 oColor;
void main() {
    vec4 texel=texture(uBaseColor,vUV)*uFactor;
    if(uShadowPass){if(texel.a<0.35)discard;oColor=vec4(1);return;}
    float edge=max(fwidth(texel.a),0.04);
    float coverage=clamp((texel.a-0.35)/edge+0.5,0.0,1.0);
    float opacity=uOpacity*vColor.a*coverage;
    if(uReveal && all(greaterThan(vPlane,uRevealBounds.xy)) && all(lessThan(vPlane,uRevealBounds.zw)))opacity*=0.22;
    // Multisample coverage smooths fine cutouts and foreground reveal without
    // the old screen-door pixel discard pattern. Ordinary alpha is the 1x fallback.
    if(opacity<0.002)discard;
    vec3 N=normalize(vNormal);if(!gl_FrontFacing)N=-N;
    float visible=shadowVisibility(vWorld.xy,vWorld.z,false);
    vec3 albedo=sceneToLinear(texture(uBaseColor,vUV).rgb)*uFactor.rgb*vColor.rgb;
    vec3 material=texture(uMetallicRoughness,vUV).rgb;
    vec3 color=sceneLighting(albedo,N,uSun,uViewDir,visible,
                              material.g*uMaterial.y,material.b*uMaterial.x,1.0);
    oColor=vec4(sceneDisplay(color),opacity);
}
)";
} // namespace

SceneryRenderer::~SceneryRenderer() {
    clear_models();
    if(instance_buffer_)glDeleteBuffers(1,&instance_buffer_);
}
void SceneryRenderer::clear_models() {
    for(auto& model:models_) {
        for(auto& mesh:model.meshes) {
            glDeleteVertexArrays(1,&mesh.vao);glDeleteBuffers(1,&mesh.vertices);glDeleteBuffers(1,&mesh.indices);
        }
        model.meshes.clear();model.materials.clear();
    }
}
bool SceneryRenderer::has_model(SceneryKind kind) const {
    const size_t i=static_cast<size_t>(kind);
    return i<models_.size() && !models_[i].meshes.empty();
}
u32 SceneryRenderer::model_count() const {
    u32 count=0;for(const auto& model:models_)if(!model.meshes.empty())++count;return count;
}
void SceneryRenderer::init(const char* asset_dir) {
    clear_models();
    if(!instance_buffer_)glGenBuffers(1,&instance_buffer_);
    if(!shader_.id()) {
        const std::string fragment=std::string("#version 330 core\n")+kShadowSamplingGLSL+kSceneLightingGLSL+kFragment;
        shader_.compile("instanced scenery",kVertex,fragment.c_str());
    }
    if(!asset_dir || !*asset_dir)return;
    for(i32 kind=0;kind<kKinds;++kind) {
        const auto& info=kSceneryModels[kind];
        const auto path=std::filesystem::path(asset_dir)/"models"/"scenery"/(std::string(info.name)+".glb");
        if(!std::filesystem::exists(path))continue;
        GltfModel source;std::string error;
        if(!source.load(path.string().c_str(),error,kind<2?1024:512)) {
            ZL_LOG("scenery %s: %s; using sprite fallback",info.name,error.c_str());continue;
        }
        auto& model=models_[static_cast<size_t>(kind)];
        const Vec3 extent=source.bounds_max-source.bounds_min;
        if(extent.x<1.0e-5f || extent.y<1.0e-5f || extent.z<1.0e-5f)continue;
        const Vec3 centre=(source.bounds_min+source.bounds_max)*0.5f;
        const Vec3 stretch{info.size.x/extent.x,info.size.z/extent.y,info.size.y/extent.z};
        for(const auto& material:source.materials) {
            Material gpu;
            const Image image=material.base_color.width?material.base_color:Image(1,1,{255,255,255,255});
            gpu.texture.create(image,TextureFilter::Trilinear,TextureWrap::Repeat);
            gpu.metallic_roughness.create(material.metallic_roughness.width ? material.metallic_roughness : Image(1,1,{255,255,255,255}),
                                           TextureFilter::Trilinear,TextureWrap::Repeat);
            gpu.metallic=material.metallic_factor;gpu.roughness=material.roughness_factor;
            std::copy(material.base_color_factor,material.base_color_factor+4,gpu.factor);
            model.materials.push_back(std::move(gpu));
        }
        if(model.materials.empty()) {
            Material gpu;gpu.texture.create(Image(1,1,{255,255,255,255}),TextureFilter::Linear,TextureWrap::Clamp);
            gpu.metallic_roughness.create(Image(1,1,{255,255,255,255}),TextureFilter::Linear,TextureWrap::Clamp);
            model.materials.push_back(std::move(gpu));
        }
        for(const auto& primitive:source.primitives) {
            if(primitive.indices.empty() || primitive.vertices.empty())continue;
            std::vector<Vertex> vertices;vertices.reserve(primitive.vertices.size());
            for(const auto& input:primitive.vertices) {
                const Vec3 p=input.position;
                const Vec3 n=normalize(Vec3{input.normal.x/stretch.x,input.normal.y/stretch.y,input.normal.z/stretch.z});
                vertices.push_back({{(p.x-centre.x)*stretch.x,-(p.z-centre.z)*stretch.z,(p.y-source.bounds_min.y)*stretch.y},
                                    {n.x,-n.z,n.y},input.uv});
            }
            Mesh mesh;mesh.count=static_cast<u32>(primitive.indices.size());
            mesh.material=clamp(primitive.material,0,static_cast<i32>(model.materials.size())-1);
            glGenVertexArrays(1,&mesh.vao);glGenBuffers(1,&mesh.vertices);glGenBuffers(1,&mesh.indices);
            glBindVertexArray(mesh.vao);glBindBuffer(GL_ARRAY_BUFFER,mesh.vertices);
            glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(vertices.size()*sizeof(Vertex)),vertices.data(),GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,mesh.indices);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,static_cast<GLsizeiptr>(primitive.indices.size()*sizeof(u32)),primitive.indices.data(),GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),nullptr);
            glEnableVertexAttribArray(1);glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offsetof(Vertex,normal)));
            glEnableVertexAttribArray(2);glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offsetof(Vertex,uv)));
            glBindBuffer(GL_ARRAY_BUFFER,instance_buffer_);
            glEnableVertexAttribArray(3);glVertexAttribPointer(3,4,GL_FLOAT,GL_FALSE,sizeof(Instance),nullptr);
            glEnableVertexAttribArray(4);glVertexAttribPointer(4,2,GL_FLOAT,GL_FALSE,sizeof(Instance),reinterpret_cast<void*>(offsetof(Instance,rotation)));
            glEnableVertexAttribArray(5);glVertexAttribPointer(5,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(Instance),reinterpret_cast<void*>(offsetof(Instance,color)));
            for(u32 i=3;i<=5;++i)glVertexAttribDivisor(i,1);
            model.meshes.push_back(mesh);
        }
    }
    glBindVertexArray(0);
    ZL_LOG("scenery: loaded %u / %d generated GLB families",model_count(),kKinds);
}
void SceneryRenderer::set_scene(const Scenery& scene,i32 width,i32 height,const std::function<f32(Vec2)>& ground_height) {
    width_=width;height_=height;columns_=(width+kBucketSize-1)/kBucketSize;rows_=(height+kBucketSize-1)/kBucketSize;
    placements_.clear();buckets_.clear();buckets_.resize(static_cast<size_t>(columns_*rows_));
    max_ground_=max_height_=max_radius_=0;
    placements_.reserve(scene.objects.size());
    for(const auto& object:scene.objects) {
        if(!object.active || !has_model(object.kind))continue;
        const f32 ground=ground_height(object.position);max_ground_=std::max(max_ground_,ground);
        const Vec3 size=scenery_model(object.kind).size;
        max_height_=std::max(max_height_,size.z*object.scale);
        max_radius_=std::max(max_radius_,(size.x+size.y)*0.5f*object.scale);
        // Chests retain their authored quarter-turn orientation; natural
        // scenery rotates independently around its physical vertical axis.
        const f32 angle=scenery_heading(object);
        Placement item{{{object.position.x,object.position.y,ground},object.scale,{std::cos(angle),std::sin(angle)},object.color},object.kind};
        const i32 x=clamp(static_cast<i32>(object.position.x)/kBucketSize,0,columns_-1);
        const i32 y=clamp(static_cast<i32>(object.position.y)/kBucketSize,0,rows_-1);
        buckets_[static_cast<size_t>(y*columns_+x)].push_back(static_cast<u32>(placements_.size()));
        placements_.push_back(item);
    }
}
void SceneryRenderer::gather(Vec2 low,Vec2 high,const std::function<bool(const Placement&)>& accept) {
    for(auto& instances:visible_)instances.clear();
    if(buckets_.empty())return;
    const i32 x0=clamp(static_cast<i32>(std::floor(low.x/kBucketSize)),0,columns_-1);
    const i32 y0=clamp(static_cast<i32>(std::floor(low.y/kBucketSize)),0,rows_-1);
    const i32 x1=clamp(static_cast<i32>(std::floor(high.x/kBucketSize)),0,columns_-1);
    const i32 y1=clamp(static_cast<i32>(std::floor(high.y/kBucketSize)),0,rows_-1);
    for(i32 y=y0;y<=y1;++y)for(i32 x=x0;x<=x1;++x)
        for(u32 index:buckets_[static_cast<size_t>(y*columns_+x)]) {
            const auto& placement=placements_[index];
            if(accept(placement))visible_[static_cast<size_t>(placement.kind)].push_back(placement.instance);
        }
}
void SceneryRenderer::bind_heightfield(u32 texture,f32 step) {
    glActiveTexture(GL_TEXTURE5);glBindTexture(GL_TEXTURE_2D,texture);
    glUniform1i(shader_.uniform("uHeights"),5);glUniform2f(shader_.uniform("uMapSize"),static_cast<f32>(width_),static_cast<f32>(height_));
    glUniform1f(shader_.uniform("uStep"),step);
}
void SceneryRenderer::submit() {
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_TRUE);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    GLint samples=0;glGetIntegerv(GL_SAMPLES,&samples);
    const bool coverage=!shadow_pass_ && samples>1;
    if(coverage)glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    else if(!shadow_pass_) {glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);}
    glUniform1i(shader_.uniform("uBaseColor"),0);
    for(size_t kind=0;kind<models_.size();++kind) {
        const auto& instances=visible_[kind];if(instances.empty())continue;
        glBindBuffer(GL_ARRAY_BUFFER,instance_buffer_);
        glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(instances.size()*sizeof(Instance)),instances.data(),GL_STREAM_DRAW);
        const auto& model=models_[kind];
        for(const auto& mesh:model.meshes) {
            const auto& material=model.materials[static_cast<size_t>(mesh.material)];
            material.texture.bind(0);glUniform4fv(shader_.uniform("uFactor"),1,material.factor);
            material.metallic_roughness.bind(1);glUniform1i(shader_.uniform("uMetallicRoughness"),1);
            glUniform2f(shader_.uniform("uMaterial"),material.metallic,material.roughness);
            glBindVertexArray(mesh.vao);
            glDrawElementsInstanced(GL_TRIANGLES,static_cast<GLsizei>(mesh.count),GL_UNSIGNED_INT,nullptr,static_cast<GLsizei>(instances.size()));
        }
    }
    glBindVertexArray(0);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_BLEND);
}
u32 SceneryRenderer::draw(const IsometricCamera& camera,const ShadowView* shadows,f32 depth_scale,
                          f32 begin,f32 end,const Vec2* reveal_lo,const Vec2* reveal_hi,u32 height_texture,f32 step) {
    if(camera.zoom<2.5f)return 0;
    Vec2 low{1.0e9f,1.0e9f},high{-1.0e9f,-1.0e9f};
    for(i32 corner=0;corner<4;++corner)for(f32 z:{0.0f,max_ground_+max_height_}) {
        const Vec2 p=camera.screen_to_world({(corner&1)?camera.viewport.x:0.0f,(corner&2)?camera.viewport.y:0.0f},z);
        low.x=std::min(low.x,p.x);low.y=std::min(low.y,p.y);high.x=std::max(high.x,p.x);high.y=std::max(high.y,p.y);
    }
    low-=Vec2{max_radius_,max_radius_};high+=Vec2{max_radius_,max_radius_};
    u32 count=0;
    gather(low,high,[&](const Placement& item) {
        const auto& a=item.instance;const Vec2 p{a.position.x,a.position.y};const auto& size=scenery_model(item.kind).size;
        const Vec2 rotated=camera.rotate(p);const f32 depth=rotated.x+rotated.y;
        if(depth<begin || depth>=end)return false;
        const f32 height=size.z*a.scale;
        if(std::max(height,size.x*a.scale)*camera.zoom<0.45f)return false;
        const Vec2 screen=camera.world_to_screen(p,a.position.z);
        const f32 radius=(size.x+size.y)*a.scale*0.5f*camera.zoom;
        if(screen.x+radius<0 || screen.x-radius>camera.viewport.x || screen.y+radius<0 ||
           screen.y-height*IsometricCamera::kZ*camera.zoom-radius>camera.viewport.y)return false;
        ++count;return true;
    });
    for(size_t kind=0;kind<visible_.size();++kind) for(auto& instance:visible_[kind]) {
        const auto& size=kSceneryModels[kind].size;
        const f32 pixels=std::max(size.x,size.z)*instance.scale*camera.zoom;
        const f32 fade=smoothstep(0.45f,2.5f,pixels);
        instance.color.a=static_cast<u8>(static_cast<f32>(instance.color.a)*fade+0.5f);
    }
    bind_view(camera,shadows,depth_scale,reveal_lo,reveal_hi,height_texture,step);
    submit();return count;
}
void SceneryRenderer::bind_view(const IsometricCamera& camera,const ShadowView* shadows,f32 depth_scale,
                                const Vec2* reveal_lo,const Vec2* reveal_hi,u32 height_texture,f32 step) {
    shadow_pass_=false;
    shader_.bind();bind_shadows(shader_,shadows);bind_heightfield(height_texture,step);
    const Camera2D plane=camera.plane_camera();const Vec2 scale=plane.clip_scale(),offset=plane.clip_offset();
    glUniform4f(shader_.uniform("uView"),scale.x,scale.y,offset.x,offset.y);
    const Vec2 rx=camera.rotate({1,0}),ry=camera.rotate({0,1});const f32 rotation[]={rx.x,rx.y,ry.x,ry.y};
    glUniformMatrix2fv(shader_.uniform("uRotation"),1,GL_FALSE,rotation);
    glUniform1f(shader_.uniform("uDepthScale"),depth_scale);glUniform1i(shader_.uniform("uShadowPass"),0);
    const Vec2 light=shadows?shadows->light:ShadowView{}.light;
    glUniform3f(shader_.uniform("uSun"),light.x,light.y,1.0f);
    const Vec2 toward=camera.unrotate({IsometricCamera::kDepthXY,IsometricCamera::kDepthXY});
    glUniform3f(shader_.uniform("uViewDir"),toward.x,toward.y,0.5f);
    glUniform1f(shader_.uniform("uOpacity"),1.0f);
    glUniform1i(shader_.uniform("uReveal"),reveal_lo && reveal_hi?1:0);
    if(reveal_lo && reveal_hi)glUniform4f(shader_.uniform("uRevealBounds"),reveal_lo->x-0.35f,reveal_lo->y-0.35f,reveal_hi->x+0.35f,reveal_hi->y+0.35f);
}
void SceneryRenderer::draw_shadow(const ShadowView& shadow,u32 height_texture,f32 step) {
    if(placements_.empty())return;
    const f32 top=max_ground_+max_height_;
    const Vec2 shift=shadow.light*top;
    Vec2 low=shadow.origin+Vec2{std::min(shift.x,0.0f),std::min(shift.y,0.0f)}-Vec2{max_radius_,max_radius_};
    Vec2 high=shadow.origin+shadow.size+Vec2{std::max(shift.x,0.0f),std::max(shift.y,0.0f)}+Vec2{max_radius_,max_radius_};
    gather(low,high,[&](const Placement& item) {
        const auto kind=item.kind;
        const auto& a=item.instance;const auto& size=scenery_model(kind).size;
        const f32 texel=shadow.size.x/static_cast<f32>(shadow.resolution);
        if(std::max(size.x,size.z)*a.scale<texel*1.5f)return false;
        const Vec2 p{a.position.x,a.position.y};const Vec2 foot=p-shadow.light*a.position.z;
        const Vec2 crown=p-shadow.light*(a.position.z+size.z*a.scale);
        const f32 radius=(size.x+size.y)*0.5f*a.scale;
        return std::max(foot.x,crown.x)+radius>=shadow.origin.x && std::min(foot.x,crown.x)-radius<=shadow.origin.x+shadow.size.x &&
               std::max(foot.y,crown.y)+radius>=shadow.origin.y && std::min(foot.y,crown.y)-radius<=shadow.origin.y+shadow.size.y;
    });
    shadow_pass_=true;
    shader_.bind();bind_shadows(shader_,&shadow);bind_heightfield(height_texture,step);
    glUniform1i(shader_.uniform("uShadowPass"),1);glUniform1i(shader_.uniform("uReveal"),0);glUniform1f(shader_.uniform("uOpacity"),1);
    submit();
}
bool SceneryRenderer::draw_preview(const IsometricCamera& camera,Vec2 point,f32 height,bool valid,u8 facing,
                                   f32 depth_scale,u32 height_texture,f32 step) {
    if(!has_model(SceneryKind::Chest))return false;
    // Preview also works in an empty scene and below the scenery LOD cutoff.
    bind_view(camera,nullptr,depth_scale,nullptr,nullptr,height_texture,step);
    for(auto& instances:visible_)instances.clear();
    const f32 angle=static_cast<f32>(facing)*1.5707963268f;
    visible_[static_cast<size_t>(SceneryKind::Chest)].push_back({{point.x,point.y,height},1,{std::cos(angle),std::sin(angle)},
                                                            valid?Color{190,220,160,255}:Color{220,80,60,255}});
    glUniform1f(shader_.uniform("uOpacity"),0.65f);submit();return true;
}

} // namespace zl
