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
        if(directory)CHECK(write_png((std::filesystem::path(directory)/(std::string(name)+".png")).c_str(),image));
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
        HumanMotion motion;motion.tool=tool;motion.work=1;motion.work_phase=0.42f;
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
        if(directory)CHECK(write_png((std::filesystem::path(directory)/(tool==HumanTool::Axe?"axe-alpha.png":"pickaxe-alpha.png")).c_str(),image));
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
    std::puts("Survival GPU checks: HUD recipes, splitting, storage, previews and four-angle tools passed");
}
