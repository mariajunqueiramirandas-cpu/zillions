// Run explicitly under Xvfb. Exercises the real HUD hit boxes and graphics,
// including recipe clicks, split stacks, storage transfers and prop previews.
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <cstdio>

#include "actor/player.h"
#include "core/jobs.h"
#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "render/figure_renderer.h"
#include "render/map_renderer.h"
#include "render/survival_ui.h"
#include "world/mapgen.h"

using namespace zl;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(0)

TileMap flat_shadow_scene(int level=0) {
    TileMap map;map.width=map.height=64;map.resolution=1;map.start={32,32};
    const size_t count=4096;
    map.terrain.assign(count,Terrain::Dirt);map.resource.assign(count,Resource::None);
    map.flags.assign(count,TileFlag::Walkable|TileFlag::Buildable);
    map.level.assign(count,static_cast<u8>(level));map.elevation.assign(count,0.5f);
    map.moisture.assign(count,0.5f);map.shore_dist.assign(count,0);
    map.scenery.ready=true;map.scenery.rebuild_index(64,64);
    return map;
}

void check_grounded_shadows(MapRenderer& renderer,JobSystem& jobs,const char* directory) {
    RenderTarget target;target.create(512,512);
    IsometricCamera camera;camera.viewport={512,512};camera.center={32.5f,32.5f};camera.zoom=192;
    for(int level:{0,2}) {
        auto map=flat_shadow_scene(level);
        map.scenery.objects.push_back({SceneryKind::Chest,0,camera.center,1,{255,255,255,255},map.index(32,32)});
        rebuild_scenery_colliders(map);renderer.set_map(map,jobs);
        camera.center_height=renderer.surface_height(camera.center,camera.zoom);
        Image facings[4];
        for(int facing=0;facing<4;++facing) {
            map.scenery.objects[0].facing=static_cast<u8>(facing);renderer.refresh_objects(map);
            for(int rotation=0;rotation<4;++rotation) {
                camera.rotation=rotation;
                const auto ground=[&](bool shadows) {
                    MapDrawOptions options;options.shadows=shadows;
                    target.bind();renderer.draw_ground(camera,options);return target.read_pixels();
                };
                const Image lit=ground(false),shaded=ground(true);
                const float edge=facing%2?0.20f:0.55f;
                for(float offset:{-0.10f,0.0f,0.10f}) {
                    const Vec2 world=camera.center+Vec2{edge+0.035f,offset};
                    const Vec2 p=camera.world_to_screen(world,renderer.surface_height(world,camera.zoom));
                    const int x=static_cast<int>(std::round(p.x)),y=static_cast<int>(std::round(p.y));
                    CHECK(int(lit.at(x,y).r)-int(shaded.at(x,y).r)>8);
                }
                const Vec2 far=camera.center+Vec2{1.35f,0.10f};
                const Vec2 p=camera.world_to_screen(far,renderer.surface_height(far,camera.zoom));
                CHECK(std::abs(int(lit.at(static_cast<int>(p.x),static_cast<int>(p.y)).r)-int(shaded.at(static_cast<int>(p.x),static_cast<int>(p.y)).r))<4);
                if(rotation==0)facings[facing]=shaded;
                if(directory&&level==0&&facing==0) {
                    renderer.draw_scenery(camera,-1e30f,1e30f);
                    CHECK(write_png((std::filesystem::path(directory)/("grounded-chest-"+std::to_string(rotation*90)+".png")).string().c_str(),target.read_pixels()));
                }
            }
        }
        // The fallback box proxy is symmetric. A real mesh also has a latch,
        // lid and hinges: those may change the shadow, within 1% of the frame.
        for(int i=0;i<2;++i) {
            int changed=0;
            for(size_t p=0;p<facings[i].pixels.size();++p)
                if(std::abs(int(facings[i].pixels[p].r)-int(facings[i+2].pixels[p].r))>2)++changed;
            CHECK(changed < (renderer.scenery_model_count() ? 512*512/100 : 10));
        }
        if(level==0) {
            camera.rotation=0;map.scenery.objects[0].facing=0;renderer.refresh_objects(map);
            const auto chest=[&]() {
                target.bind();renderer.draw_ground(camera,{});
                // Draw the chest alone, retaining any offscreen/sunward caster.
                renderer.draw_scenery(camera,64.9f,1e30f);
                return target.read_pixels();
            };
            const Image unoccluded=chest();
            const Vec2 tree=camera.center+ShadowView{}.light*3.0f;
            map.scenery.objects.push_back({SceneryKind::Conifer,0,tree,1,{255,255,255,255},map.index(29,31)});
            renderer.refresh_objects(map);
            const Image occluded=chest();
            const Vec2 top=camera.world_to_screen(camera.center,camera.center_height+0.4f);
            int shaded_pixels=0;
            for(int y=static_cast<int>(top.y)-16;y<=static_cast<int>(top.y)+16;++y)
                for(int x=static_cast<int>(top.x)-16;x<=static_cast<int>(top.x)+16;++x)
                    shaded_pixels+=int(unoccluded.at(x,y).r)-int(occluded.at(x,y).r)>8;
            CHECK(shaded_pixels>20);
        }
    }
    CHECK(glGetError()==GL_NO_ERROR);
}

void capture_tool_strikes(MapRenderer& renderer,FigureRenderer& figures,JobSystem& jobs,const char* directory) {
    RenderTarget target;target.create(512,512);
    IsometricCamera camera;camera.viewport={512,512};camera.zoom=128;
    for(const auto tool:{HumanTool::Axe,HumanTool::Pickaxe}) {
        auto map=flat_shadow_scene();const Vec2 node{32.5f,32.5f};
        const auto kind=tool==HumanTool::Axe?SceneryKind::Broadleaf:SceneryKind::Boulder;
        const auto& shape=scenery_model(kind);
        const Vec2 standing=node-Vec2{shape.trunk_radius+shape.half_length+0.36f,0};
        map.scenery.objects.push_back({kind,0,node,1,{255,255,255,255},map.index(32,32)});
        rebuild_scenery_colliders(map);renderer.set_map(map,jobs);
        camera.center=(standing+node)*0.5f;camera.center_height=renderer.surface_height(camera.center,128)+0.9f;
        for(int rotation=0;rotation<4;++rotation)for(float phase:{0.1f,0.45f,0.72f}) {
            camera.rotation=rotation;
            HumanMotion motion;motion.heading=0;motion.tool=tool;motion.work=1;motion.work_phase=phase;
            Figure figure;build_human(hero_look(),motion,figure);
            target.bind();renderer.draw_ground(camera,{});
            const Vec2 r=camera.rotate(standing);const float depth=r.x+r.y;
            renderer.draw_scenery(camera,-1e30f,depth);
            FigureDraw item{&figure,standing,0,0,renderer.surface_height(standing,128)};
            glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
            figures.draw(&item,1,camera.plane_camera(),&camera,renderer.shadow_view(),renderer.depth_scale());glDepthMask(GL_TRUE);
            const Vec2 feet=camera.project(standing,item.height);
            const SeeThrough reveal{feet-Vec2{0.7f,1.6f},feet+Vec2{0.7f,0.2f}};
            renderer.draw_scenery(camera,depth,1e30f,&reveal);
            if(directory) {
                const auto name=std::string(tool==HumanTool::Axe?"chopping":"mining")+"-"+std::to_string(rotation*90)+"-"+std::to_string(static_cast<int>(phase*100))+".png";
                CHECK(write_png((std::filesystem::path(directory)/name).string().c_str(),target.read_pixels()));
            }
            CHECK(glGetError()==GL_NO_ERROR);
        }
    }
}

int main() {
    WindowConfig config;config.width=1024;config.height=720;config.visible=false;Window window(config);
    JobSystem jobs(2);MapGenParams params;params.seed=7;params.size=128;params.resolution=1;
    auto map=generate_map(params,jobs);Survival game;game.reset(map);
    MapRenderer renderer;renderer.init(jobs,ZL_ASSET_DIR);renderer.set_map(map,jobs);
    Player player;CHECK(player.spawn(map));
    IsometricCamera camera;camera.viewport={1024,720};camera.center=player.position();camera.zoom=96;
    camera.center_height=renderer.surface_height(player.position(),camera.zoom)+0.5f;
    FigureRenderer figures;figures.init();SurvivalUI hud;hud.init();
    RenderTarget target;target.create(1024,720);
    const char* directory=std::getenv("ZL_SURVIVAL_CAPTURE_DIR");
    if(directory)std::filesystem::create_directories(directory);
    const auto capture=[&](const char* name,HumanTool tool=HumanTool::None,float phase=0.0f) {
        target.bind();MapDrawOptions options;renderer.draw_ground(camera,options);
        const Vec2 rotated=camera.rotate(player.position());const float depth=rotated.x+rotated.y;
        renderer.draw_scenery(camera,-1.0e30f,depth);
        HumanMotion motion=player.motion();motion.tool=tool;motion.work=tool==HumanTool::None?0.0f:1.0f;motion.work_phase=phase;
        Figure figure;build_human(player.look,motion,figure);
        FigureDraw actor{&figure,player.position(),0,0,renderer.surface_height(player.position(),camera.zoom)};
        glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
        figures.draw(&actor,1,camera.plane_camera(),&camera,renderer.shadow_view(),renderer.depth_scale());glDepthMask(GL_TRUE);
        renderer.draw_scenery(camera,depth,1.0e30f);
        Input input;hud.draw(camera.viewport,input,game,nullptr,true);
        CHECK(glGetError()==GL_NO_ERROR);
        const auto image=target.read_pixels();
        // The HUD must actually cover terrain and contain bright text.
        CHECK(image.at(25,25).r<70);int bright=0;
        for(int y=110;y<145;++y)for(int x=30;x<370;++x)if(image.at(x,y).r>150)++bright;
        if(hud.open)CHECK(bright>50);
        if(directory)CHECK(write_png((std::filesystem::path(directory)/(std::string(name)+".png")).string().c_str(),image));
        return image;
    };
    game.bag.add(Item::Wood,30);game.bag.add(Item::Stone,12);game.bag.add(Item::Fiber,18);
    hud.open=true;
    const auto click=[&](float x,float y,bool right=false,bool shift=false) {
        Input input;input.mouse={x,y};input.button_pressed[static_cast<int>(right?MouseButton::Right:MouseButton::Left)]=true;
        input.key_down[static_cast<int>(Key::LeftShift)]=shift;hud.update(input,camera.viewport,game);
    };
    click(650,200,false,true);CHECK(game.crafting()==0);
    for(int i=0;i<240;++i)game.step(1.0f/60);
    CHECK(game.bag.count(Item::Rope)==5);
    click(650,280);for(int i=0;i<100;++i)game.step(1.0f/60);CHECK(game.axe);
    click(650,355);for(int i=0;i<100;++i)game.step(1.0f/60);CHECK(game.pickaxe);
    click(650,431);for(int i=0;i<130;++i)game.step(1.0f/60);CHECK(game.bag.count(Item::Chest)==1);
    capture("inventory");
    // Right click splits; closing returns the held stack without loss.
    const int wood=game.bag.count(Item::Wood);click(50,190,true);CHECK(hud.hand.count>0);
    CHECK(hud.close(game));CHECK(game.bag.count(Item::Wood)==wood);
    hud.placing=true;
    for(int turn=1;turn<=4;++turn) {
        Input rotate;rotate.key_pressed[static_cast<int>(Key::T)]=true;
        hud.update(rotate,camera.viewport,game);
        CHECK(hud.placement_facing==turn%4);
    }
    hud.placing=false;
    Vec2 point;bool placed=false;
    for(int y=-2;y<=2&&!placed;++y)for(int x=-2;x<=2&&!placed;++x) {
        point=player.position()+Vec2{static_cast<float>(x),static_cast<float>(y)};
        if(game.can_place_chest(map,player.position(),point)) {
            target.bind();renderer.draw(camera,{});renderer.draw_chest_preview(camera,point,true);CHECK(glGetError()==GL_NO_ERROR);
            placed=game.place_chest(map,player.position(),point);
        }
    }
    CHECK(placed);renderer.refresh_objects(map);
    CHECK(renderer.decor_count()>0);hud.open=true;hud.chest=0;
    int stone=game.bag.count(Item::Stone);
    for(int slot=0;slot<20;++slot)click(50+static_cast<float>(slot%5)*66,190+static_cast<float>(slot/5)*66,false,true);
    CHECK(game.bag.count(Item::Stone)==0&&game.chests[0].inventory.count(Item::Stone)==stone);
    capture("storage");hud.close(game);
    for(int rotation=0;rotation<4;++rotation) {
        camera.rotation=rotation;
        capture(("axe-"+std::to_string(rotation*90)).c_str(),HumanTool::Axe,0.42f);
        capture(("pickaxe-"+std::to_string(rotation*90)).c_str(),HumanTool::Pickaxe,0.72f);
    }
    camera.rotation=0;camera.center=point;camera.center_height=renderer.surface_height(point,camera.zoom);
    camera.zoom=160;
    Image facings[4];
    auto& chest_object=map.scenery.objects[game.chests[0].object];
    for(int facing=0;facing<4;++facing) {
        chest_object.facing=static_cast<u8>(facing);rebuild_scenery_colliders(map);renderer.refresh_objects(map);
        for(const auto& collider:map.scenery.colliders)if(collider.object==game.chests[0].object) {
            if(facing%2)CHECK(collider.a.x==collider.b.x&&collider.a.y!=collider.b.y);
            else CHECK(collider.a.y==collider.b.y&&collider.a.x!=collider.b.x);
        }
        facings[facing]=capture(("chest-facing-"+std::to_string(facing)).c_str());
        for(int before=0;before<facing;++before) {
            int changed=0;
            for(int y=280;y<390;++y)for(int x=410;x<615;++x) {
                const auto a=facings[facing].at(x,y),b=facings[before].at(x,y);
                if(std::abs(int(a.r)-int(b.r))+std::abs(int(a.g)-int(b.g))+std::abs(int(a.b)-int(b.b))>20)++changed;
            }
            CHECK(changed>100);
        }
        target.bind();renderer.draw(camera,{});renderer.draw_chest_preview(camera,point,true,static_cast<u8>(facing));CHECK(glGetError()==GL_NO_ERROR);
    }
    chest_object.facing=0;rebuild_scenery_colliders(map);renderer.refresh_objects(map);
    for(int rotation=0;rotation<4;++rotation) {
        camera.rotation=rotation;
        capture(("chest-camera-"+std::to_string(rotation*90)).c_str());
    }
    // A transparent tool canvas must not become a rectangular ground shadow.
    // Isolate the tool on transparent black; opaque sprite + real shadow cover
    // a small fraction of the frame, unlike the full-card alpha regression.
    RenderTarget isolated;isolated.create(256,256);
    Camera2D tool_camera;tool_camera.viewport={256,256};tool_camera.center={0,-0.6f};tool_camera.zoom=160;
    for(const auto tool:{HumanTool::Axe,HumanTool::Pickaxe}) {
        HumanMotion motion;motion.tool=tool;motion.work=1;motion.work_phase=0.42f;motion.heading=0;
        Figure figure;build_human(player.look,motion,figure);
        std::erase_if(figure.parts,[](const FigurePart& part){return part.material<FigureMaterial::ToolAxe;});
        CHECK(figure.parts.size()==1);
        FigureDraw item{&figure,{0,0}};
        isolated.bind();glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        figures.draw(&item,1,tool_camera);
        // RenderTarget::read_pixels intentionally flattens screenshot alpha;
        // read raw RGBA here so this assertion measures actual transparency.
        Image image(256,256);glReadPixels(0,0,256,256,GL_RGBA,GL_UNSIGNED_BYTE,image.bytes());
        for(int y=0;y<128;++y)std::swap_ranges(image.pixels.begin()+y*256,image.pixels.begin()+(y+1)*256,image.pixels.begin()+(255-y)*256);
        int occupied=0;
        for(const auto c:image.pixels) {
            occupied+=c.a>8;
            CHECK(int(c.r)<=int(c.a)+1&&int(c.g)<=int(c.a)+1&&int(c.b)<=int(c.a)+1);
        }
        std::printf("tool cutout %d: %d occupied pixels\n",static_cast<int>(tool),occupied);
        if(directory)CHECK(write_png((std::filesystem::path(directory)/(tool==HumanTool::Axe?"axe-alpha.png":"pickaxe-alpha.png")).string().c_str(),image));
        CHECK(occupied>40&&occupied<8192);
        CHECK(image.at(0,0).a==0&&image.at(255,255).a==0);
        CHECK(glGetError()==GL_NO_ERROR);
    }
    // A clean art-review fixture shows every new world prop beside the actor.
    const Vec2 anchor=player.position();
    map.scenery.objects.clear();
    for(int facing=0;facing<4;++facing) {
        const float offset=static_cast<float>(facing*2-3);
        const Vec2 position=anchor+Vec2{offset,-offset};
        map.scenery.objects.push_back({SceneryKind::Chest,0,position,1,{255,255,255,255},
            map.index(static_cast<int>(position.x),static_cast<int>(position.y)),true,static_cast<u8>(facing)});
    }
    const SceneryKind props[]={SceneryKind::Branch,SceneryKind::Stump,SceneryKind::Rubble,SceneryKind::Branch};
    for(int i=0;i<4;++i) {
        const float offset=static_cast<float>(i*2-3);
        const Vec2 position=anchor+Vec2{offset+1.5f,1.5f-offset};
        map.scenery.objects.push_back({props[i],static_cast<u32>(i==3?1:0),position,
            props[i]==SceneryKind::Branch?0.65f:1.0f,{255,255,255,255},map.index(static_cast<int>(position.x),static_cast<int>(position.y))});
    }
    rebuild_scenery_colliders(map);renderer.refresh_objects(map);
    camera.rotation=0;camera.center=anchor;camera.center_height=renderer.surface_height(anchor,96);camera.zoom=96;
    capture("generated-world-props",HumanTool::Axe,0.42f);
    check_grounded_shadows(renderer,jobs,directory);
    capture_tool_strikes(renderer,figures,jobs,directory);
    std::puts("Survival GPU checks: HUD recipes, splitting, storage, previews and four-angle tools passed");
}
