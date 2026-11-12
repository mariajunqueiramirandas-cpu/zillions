// Real generated assets, physical scale and full-size trunk clearance.
#include <cmath>
#include <cstdio>
#include <string>

#include "actor/player.h"
#include "asset/gltf.h"
#include "world/mapgen.h"

using namespace zl;
int main() {
    int failures=0;
    const auto check=[&](bool condition,const char* message) {
        if(!condition){++failures;std::printf("FAIL: %s\n",message);}
    };
    check(MapGenParams{}.size==4096,"default world is 4096 tiles per side");
    const f32 human_height=Player{}.body_height();
    const f32 oak_height=scenery_model(SceneryKind::Broadleaf).size.z;
    const f32 pine_height=scenery_model(SceneryKind::Conifer).size.z;
    check(oak_height>4.0f*human_height && oak_height<6.0f*human_height,"oak is substantial without overwhelming the player view");
    check(pine_height>oak_height && pine_height<8.0f*human_height,"pine stays taller than oak at a readable play scale");
    for(const auto& info:kSceneryModels) {
        GltfModel model;std::string error;
        const std::string path=std::string(ZL_ASSET_DIR)+"/models/scenery/"+info.name+".glb";
        const bool loaded=model.load(path.c_str(),error,128);
        check(loaded,info.name);
        if(!loaded){std::printf("%s\n",error.c_str());continue;}
        check(!model.skinned && !model.primitives.empty(),"scenery is a static usable mesh");
        const Vec3 extent=model.bounds_max-model.bounds_min;
        check(extent.x>0 && extent.y>0 && extent.z>0,"mesh has nonzero bounds in every dimension");
        size_t triangles=0;
        for(const auto& primitive:model.primitives) {
            check(primitive.indices.size()%3==0,"triangle topology");
            triangles+=primitive.indices.size()/3;
            for(u32 index:primitive.indices)check(index<primitive.vertices.size(),"valid mesh indices");
            for(const auto& vertex:primitive.vertices)check(std::isfinite(vertex.position.x+vertex.position.y+vertex.position.z),"finite geometry");
        }
        check(triangles>20 && triangles<=8500,"bounded instance triangle budget");
        bool textured=false;
        for(const auto& material:model.materials)textured=textured || material.base_color.width>0;
        check(textured,"embedded base-colour texture loads");
        std::printf("%-10s %zu triangles\n",info.name,triangles);
    }
    TileMap map;map.width=map.height=32;map.level.assign(1024,0);map.terrain.assign(1024,Terrain::Forest);
    map.flags.assign(1024,TileFlag::Walkable);
    map.scenery.ready=true;
    map.scenery.objects.push_back({SceneryKind::Broadleaf,0,{16,16},1,{255,255,255,255},528});
    rebuild_scenery_colliders(map);
    check(map.scenery.colliders.size()==1,"tree has one trunk collider");
    check(map.scenery.overlaps({16,16},0.26f,0,1.36f),"trunk blocks a human body");
    check(!map.scenery.overlaps({18,16},0.26f,0,1.36f),"player fits underneath the wide canopy");
    ObjectContact hit;
    check(map.scenery.sweep({10,16},{12,0},0.26f,0,1.36f,hit),"fast motion cannot tunnel through a mature trunk");
    check(!map.scenery.sweep({10,16},{12,0},0.26f,0,1.36f,hit,0),"interaction ray can ignore its target");
    std::printf("scenery asset checks: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
