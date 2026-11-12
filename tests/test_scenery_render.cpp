// GPU checks for real static meshes, rotation, shadows, culling and harvesting.
#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "core/jobs.h"
#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "platform/window.h"
#include "render/map_renderer.h"

using namespace zl;
int main(int argc,char** argv) {
    int failures=0;
    const auto check=[&](bool value,const char* message){if(!value){++failures;std::printf("FAIL: %s\n",message);}};
    WindowConfig config;config.visible=false;config.width=384;config.height=384;Window window(config);
    JobSystem jobs(2);MapRenderer renderer;renderer.init(jobs,ZL_ASSET_DIR);
    check(renderer.scenery_model_count()==15,"all fifteen generated model families loaded");
    TileMap map;map.width=map.height=64;map.resolution=1;map.start={32,32};
    map.terrain.assign(4096,Terrain::Dirt);map.resource.assign(4096,Resource::None);map.level.assign(4096,0);
    map.flags.assign(4096,TileFlag::Walkable|TileFlag::Buildable);map.elevation.assign(4096,0.3f);
    map.moisture.assign(4096,0.5f);map.shore_dist.assign(4096,0);map.scenery.ready=true;
    renderer.set_map(map,jobs);
    IsometricCamera camera;camera.viewport={384,384};camera.center={32.5f,32.5f};
    RenderTarget target;target.create(384,384);
    Image gallery(384*5,384*3);
    for(i32 kind=0;kind<15;++kind) {
        const auto& info=kSceneryModels[kind];
        map.scenery.objects={{{static_cast<SceneryKind>(kind)},0,camera.center,1,{255,255,255,255},map.index(32,32)}};
        renderer.refresh_objects(map);
        camera.zoom=std::min(220.0f,270.0f/std::max(info.size.z,std::max(info.size.x,info.size.y)));
        camera.center_height=renderer.surface_height(camera.center,camera.zoom)+info.size.z*0.43f;
        Image views[4];
        for(i32 angle=0;angle<4;++angle) {
            camera.rotation=angle;target.bind();
            if(angle==0) {
                renderer.draw_ground(camera,{});
                std::vector<f32> ground(384*384),solid(384*384);
                glReadPixels(0,0,384,384,GL_DEPTH_COMPONENT,GL_FLOAT,ground.data());
                renderer.draw_scenery(camera,-1e30f,1e30f);
                glReadPixels(0,0,384,384,GL_DEPTH_COMPONENT,GL_FLOAT,solid.data());
                size_t written=0;for(size_t p=0;p<ground.size();++p)if(solid[p]<ground[p]-0.000001f)++written;
                check(written>20,"model fragments write depth in front of the terrain");
            } else renderer.draw(camera,{});
            views[angle]=target.read_pixels();
            check(renderer.sprites_drawn()==1,"one mesh instance submitted at each angle");
            check(glGetError()==GL_NO_ERROR,"mesh and shadow passes are valid OpenGL");
        }
        size_t changed=0;for(size_t p=0;p<views[0].pixels.size();++p) {
            const auto a=views[0].pixels[p],b=views[1].pixels[p];
            if(std::abs(int(a.r)-int(b.r))+std::abs(int(a.g)-int(b.g))+std::abs(int(a.b)-int(b.b))>10)++changed;
        }
        check(changed>30,"camera rotation reveals a different 3D silhouette");
        for(i32 y=0;y<384;++y)for(i32 x=0;x<384;++x)gallery.at((kind%5)*384+x,(kind/5)*384+y)=views[0].at(x,y);
        map.scenery.objects[0].active=false;renderer.refresh_objects(map);target.bind();renderer.draw(camera,{});
        check(renderer.sprites_drawn()==0,"depleted meshes disappear after an object refresh");
        std::printf("%s: four views, mesh depth, removal\n",info.name);
    }
    if(argc>1){std::filesystem::create_directories(argv[1]);check(write_png((std::filesystem::path(argv[1])/"scenery-gallery.png").string().c_str(),gallery),"gallery capture");}
    // Anchor outside the viewport, but a towering canopy still intersects it.
    map.scenery.objects={{{SceneryKind::Conifer},0,{50.5f,50.5f},1,{255,255,255,255},map.index(50,50)}};
    renderer.refresh_objects(map);camera.rotation=0;camera.zoom=20;camera.center_height=0;
    check(camera.world_to_screen({50.5f,50.5f},renderer.surface_height({50.5f,50.5f},20)).y>384,"fixture roots are below the viewport");
    target.bind();renderer.draw(camera,{});check(renderer.sprites_drawn()==1,"offscreen roots do not cull a visible crown");
    camera.center={-300,-300};target.bind();renderer.draw(camera,{});check(renderer.sprites_drawn()==0,"distant buckets are culled");
    map.scenery.objects.clear();renderer.refresh_objects(map);camera.center={32.5f,32.5f};
    for(f32 zoom:{64.0f,1.0f}) {
        camera.zoom=zoom;target.bind();renderer.draw(camera,{});
        renderer.draw_chest_preview(camera,camera.center,true,1);
        check(glGetError()==GL_NO_ERROR,"placement previews work in an empty scene at every zoom");
    }
    std::printf("scenery GPU checks: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
