#include <cstdio>
#include <cstdlib>

#include "actor/human.h"
#include "actor/player.h"
#include "core/jobs.h"
#include "survival/survival.h"
#include "world/mapgen.h"

using namespace zl;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(0)

TileMap flat() {
    TileMap m; m.width=m.height=32;m.resolution=1;m.start={16,16};
    const size_t n=1024;
    m.terrain.assign(n,Terrain::Grass);m.resource.assign(n,Resource::None);
    m.flags.assign(n,TileFlag::Walkable|TileFlag::Buildable);m.level.assign(n,0);
    m.elevation.assign(n,0.4f);m.moisture.assign(n,0.5f);m.shore_dist.assign(n,0);
    m.scenery.ready=true;m.scenery.rebuild_index(32,32);
    return m;
}
void inventory() {
    Inventory a(2),b(1);
    CHECK(a.add(Item::Wood,75));CHECK(!a.add(Item::Wood,26));CHECK(a.count(Item::Wood)==75);
    Stack hand;
    a.click(0,hand,true);CHECK(hand.count==25&&a.count(Item::Wood)==50);
    a.click(1,hand,true);CHECK(hand.count==24&&a.count(Item::Wood)==51);
    a.click(0,hand,false);CHECK(hand.count==0&&a.count(Item::Wood)==75);
    CHECK(a.transfer(b,0,50)==49);CHECK(a.count(Item::Wood)+b.count(Item::Wood)==75);
    CHECK(a.transfer(b,1,50)==1);CHECK(b.count(Item::Wood)==50);
    CHECK(!a.remove(Item::Stone,1));CHECK(!a.add(Item::None,2));
    CHECK(a.transfer(a,0,10)==0);
}
void crafting_and_storage() {
    auto m=flat();Survival g;g.reset(m);
    CHECK(!g.start_craft(1));
    CHECK(g.bag.add(Item::Wood,30));CHECK(g.bag.add(Item::Stone,10));CHECK(g.bag.add(Item::Fiber,18));
    CHECK(g.start_craft(0,4));for(int i=0;i<200;++i)g.step(1.0f/60);
    CHECK(g.bag.count(Item::Rope)==4);CHECK(g.bag.count(Item::Fiber)==6);
    CHECK(g.start_craft(1));CHECK(!g.start_craft(2));
    for(int i=0;i<100;++i)g.step(1.0f/60);
    CHECK(g.axe&&!g.can_craft(1));CHECK(g.bag.count(Item::Wood)==27);
    CHECK(g.start_craft(2));for(int i=0;i<100;++i)g.step(1.0f/60);
    CHECK(g.pickaxe);
    CHECK(g.start_craft(3));for(int i=0;i<130;++i)g.step(1.0f/60);
    CHECK(g.bag.count(Item::Chest)==1);
    const Vec2 player{16.5f,16.5f}, point{18.5f,16.5f};
    CHECK(!g.place_chest(m,player,{16.5f,16.5f}));CHECK(!g.place_chest(m,player,{25.5f,25.5f}));
    m.terrain[static_cast<size_t>(m.index(17,16))]=Terrain::Water;
    CHECK(!g.can_place_chest(m,player,point));
    m.terrain[static_cast<size_t>(m.index(17,16))]=Terrain::Grass;
    CHECK(g.place_chest(m,player,point));CHECK(g.bag.count(Item::Chest)==0);
    CHECK(g.chests.size()==1&&g.chests[0].inventory.slots().size()==24);
    CHECK(!m.buildable(18,16));CHECK(m.scenery.overlaps(point,0.2f,0,1.4f));
    CHECK(g.chest_at(g.chests[0].object)==0);
    const int total=g.bag.count(Item::Wood);
    for(int i=0;i<20;++i)g.bag.transfer(g.chests[0].inventory,i,50);
    CHECK(g.chests[0].inventory.count(Item::Wood)==total);
    // Ingredients moved away while crafting cannot be lost or create output.
    g.bag.add(Item::Fiber,3);CHECK(g.start_craft(0));g.bag.remove(Item::Fiber,g.bag.count(Item::Fiber));
    g.step(1);CHECK(g.bag.count(Item::Rope)==0);
}
void harvesting() {
    auto m=flat();const int tile=m.index(18,16);
    m.terrain[static_cast<size_t>(tile)]=Terrain::Forest;m.flags[static_cast<size_t>(tile)]=0;
    m.scenery.objects.push_back({SceneryKind::Broadleaf,0,{18.25f,16.5f},1,{255,255,255,255},tile});
    m.scenery.objects.push_back({SceneryKind::Conifer,0,{18.75f,16.5f},1,{255,255,255,255},tile});
    rebuild_scenery_colliders(m);
    Survival g;g.reset(m);const int a=g.node_at(0),b=g.node_at(1);CHECK(a>=0&&b>=0);
    const Vec2 player{17.5f,16.5f};
    CHECK(g.harvest_problem(m,player,a));g.axe=true;CHECK(!g.harvest_problem(m,player,a));
    CHECK(g.pick_node(m,{18.2f,16.5f})==a);
    g.harvest(m,player,a,true,false,0.4f);CHECK(g.harvest_progress()>0);
    g.harvest(m,player,a,true,true,0.5f);CHECK(g.active_node()==-1&&g.bag.count(Item::Wood)==0);
    for(int i=0;i<520;++i)g.harvest(m,player,a,true,false,1.0f/60);
    CHECK(g.bag.count(Item::Wood)==10&&g.node_at(0)==-1);
    CHECK(m.scenery.objects[0].kind==SceneryKind::Stump);CHECK(!m.buildable(18,16));
    for(const auto& c:m.scenery.colliders)CHECK(c.object!=0);
    for(int i=0;i<520;++i)g.harvest(m,player,b,true,false,1.0f/60);
    CHECK(g.bag.count(Item::Wood)==20&&m.buildable(18,16));CHECK(m.terrain[static_cast<size_t>(tile)]==Terrain::Dirt);
    // Full bags never consume a strike or a node's remaining supply.
    auto n=flat();n.scenery.objects.push_back({SceneryKind::Boulder,0,{18.7f,16.5f},1,{255,255,255,255},tile});
    rebuild_scenery_colliders(n);Survival full;full.reset(n);full.pickaxe=true;full.bag=Inventory(1);full.bag.add(Item::Fiber,50);
    const int rock=full.node_at(0);const int remaining=full.nodes[static_cast<size_t>(rock)].remaining;
    full.harvest(n,player,rock,true,false,2);CHECK(full.nodes[static_cast<size_t>(rock)].remaining==remaining);
    full.bag.remove(Item::Fiber,50);
    n.scenery.objects.push_back({SceneryKind::Broadleaf,0,{18.0f,16.5f},1,{255,255,255,255},tile});
    rebuild_scenery_colliders(n);CHECK(!full.in_reach(n,player,0));
    n.scenery.objects.back().active=false;rebuild_scenery_colliders(n);CHECK(full.in_reach(n,player,0));
    n.level[static_cast<size_t>(tile)]=1;
    CHECK(!full.in_reach(n,player,0));CHECK(full.harvest_problem(n,player,rock));
}
void bootstrap() {
    JobSystem jobs(2);
    for(const int resolution:{1,2,4}) for(const u64 seed:{1ULL,7ULL,19ULL}) {
        MapGenParams params;params.seed=seed;params.resolution=resolution;params.size=64*resolution;
        auto m=generate_map(params,jobs);Survival g;g.reset(m);
        int wood=0,stone=0,fiber=0;
        for(const auto& n:g.nodes) {
            const auto& o=m.scenery.objects[n.object];
            if(n.tool!=Item::None||length(o.position-Vec2{static_cast<float>(m.start.x)+0.5f,static_cast<float>(m.start.y)+0.5f})>8)continue;
            if(n.item==Item::Wood)wood+=n.remaining;
            if(n.item==Item::Stone)stone+=n.remaining;
            if(n.item==Item::Fiber)fiber+=n.remaining;
        }
        CHECK(wood>=6&&stone>=6&&fiber>=6);
    }
}
void tool_poses() {
    for(u64 seed=0;seed<16;++seed)for(const auto tool:{HumanTool::Axe,HumanTool::Pickaxe})for(int i=0;i<100;++i) {
        HumanMotion motion;motion.tool=tool;motion.work=1;motion.work_phase=static_cast<float>(i)/100;
        auto look=seed==0?hero_look():random_look(seed);look.beard=true;look.hood_up=(seed%2)==0;
        const auto skeleton=pose_human(look,motion);Figure figure;build_human(look,motion,figure);
        CHECK(figure.parts.size()<static_cast<size_t>(kMaxFigureParts));
        for(int arm=0;arm<2;++arm) {CHECK(std::fabs(length(skeleton.elbow[arm]-skeleton.shoulder[arm])-kUpperArmLength)<0.001f);CHECK(std::fabs(length(skeleton.wrist[arm]-skeleton.elbow[arm])-kForearmLength)<0.001f);}
        Vec2 lo,hi;CHECK(figure_screen_bounds(figure,lo,hi));CHECK(std::isfinite(lo.x)&&std::isfinite(hi.y));
    }
}
void first_camp_loop() {
    JobSystem jobs(2);MapGenParams params;params.size=128;params.resolution=1;params.seed=7;
    auto m=generate_map(params,jobs);Survival g;g.reset(m);Player player;CHECK(player.spawn(m));
    // Exercise actual resource strikes and recipes, starting with an empty bag.
    const auto collect=[&](Item item,int amount,Item tool) {
        for(int i=0;i<static_cast<int>(g.nodes.size())&&g.bag.count(item)<amount;++i) {
            auto& n=g.nodes[static_cast<size_t>(i)];if(n.item!=item||n.tool!=tool||n.remaining<=0)continue;
            const Vec2 target=m.scenery.objects[n.object].position;
            Vec2 stance;bool found=false;
            for(int a=0;a<16&&!found;++a) {
                const float angle=static_cast<float>(a)*kPi/8;
                stance=target+Vec2{std::cos(angle),std::sin(angle)}*1.2f;
                found=can_stand(m,stance,player.body_radius(),player.body_height())&&g.in_reach(m,stance,n.object);
            }
            if(!found)continue;
            CHECK(player.spawn(m,stance));
            for(int frame=0;frame<3000&&g.bag.count(item)<amount&&n.remaining>0;++frame)
                g.harvest(m,player.position(),i,true,false,1.0f/60);
        }
        CHECK(g.bag.count(item)>=amount);g.cancel_harvest();
    };
    const auto craft=[&](int recipe,int times=1) {CHECK(g.start_craft(recipe,times));for(int frame=0;frame<3000&&g.crafting()>=0;++frame)g.step(1.0f/60);CHECK(g.crafting()<0);};
    collect(Item::Wood,6,Item::None);collect(Item::Stone,6,Item::None);collect(Item::Fiber,12,Item::None);
    craft(0,4);craft(1);craft(2);CHECK(g.axe&&g.pickaxe);
    collect(Item::Wood,12,Item::Axe);collect(Item::Stone,5,Item::Pickaxe);craft(3);
    CHECK(player.spawn(m));const Vec2 anchor=player.position();bool placed=false;
    for(int y=-2;y<=2&&!placed;++y)for(int x=-2;x<=2&&!placed;++x)placed=g.place_chest(m,anchor,anchor+Vec2{static_cast<float>(x),static_cast<float>(y)});
    CHECK(placed&&g.chests.size()==1);
    for(int i=0;i<20;++i)g.bag.transfer(g.chests[0].inventory,i,50);
    CHECK(g.chests[0].inventory.count(Item::Stone)>=5);
}
int main() {inventory();crafting_and_storage();harvesting();bootstrap();tool_poses();first_camp_loop();std::puts("Survival: inventory, crafting, gathering, bootstrap, storage, tool poses and first camp loop passed");}
