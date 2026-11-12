// Distribution and traversal checks, including real generated maps.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "actor/player.h"
#include "core/jobs.h"
#include "world/mapgen.h"

using namespace zl;
namespace {
int failures=0;
#define CHECK(condition) do {if(!(condition)){++failures;std::printf("FAIL line %d: %s\n",__LINE__,#condition);}} while(0)
bool tree(SceneryKind kind){return kind==SceneryKind::Broadleaf || kind==SceneryKind::Conifer;}

TileMap plain(Terrain terrain=Terrain::Grass) {
    TileMap map;map.width=map.height=256;map.resolution=1;map.seed=719;map.start={128,128};
    map.terrain.assign(65536,terrain);map.resource.assign(65536,Resource::None);map.level.assign(65536,0);
    map.flags.assign(65536,TileFlag::Walkable|TileFlag::Buildable);
    map.elevation.assign(65536,0.30f);map.moisture.assign(65536,0.5f);map.shore_dist.assign(65536,0);
    map.biomes.create(map.width,map.height);return map;
}

void density_and_spacing() {
    TileMap map=plain();
    for(i32 y=0;y<map.biomes.height;++y)for(i32 x=0;x<map.biomes.width;++x)
        map.biomes.cells[static_cast<size_t>(y*map.biomes.width+x)].woodland=smoothstep(16,240,static_cast<f32>(x*BiomeField::kSpacing));
    const Scenery scene=build_scenery(map);
    int bins[8]{};std::vector<const SceneryObject*> grid(104*104,nullptr);
    for(const auto& object:scene.objects)if(tree(object.kind)) {
        ++bins[std::min(7,static_cast<i32>(object.position.x)/32)];
        const i32 x=static_cast<i32>(object.position.x/2.5f),y=static_cast<i32>(object.position.y/2.5f);
        grid[static_cast<size_t>(y*104+x)]=&object;
        CHECK(length(object.position-Vec2{128,128})>=8.0f);
    }
    std::printf("forest gradient trees by equal-area strip:");for(int n:bins)std::printf(" %d",n);std::printf("\n");
    CHECK(bins[0]<bins[2] && bins[2]<bins[4] && bins[4]<bins[6]);
    CHECK(bins[6]>900); // dense cores exceed the former one-per-16-tiles ceiling
    for(i32 y=0;y<104;++y)for(i32 x=0;x<104;++x)if(const auto* object=grid[static_cast<size_t>(y*104+x)]) {
        for(i32 ny=std::max(0,y-1);ny<=std::min(103,y+1);++ny)for(i32 nx=std::max(0,x-1);nx<=std::min(103,x+1);++nx) {
            const auto* other=grid[static_cast<size_t>(ny*104+nx)];if(!other || other==object)continue;
            CHECK(length(other->position-object->position)>=1.999f);
            const Vec2 gap=(other->position+object->position)*0.5f;
            CHECK(!scene.overlaps(gap,Player::kRadius,0,Player{}.body_height()));
        }
    }
    map=plain(Terrain::Forest);
    for(auto& cell:map.biomes.cells)cell={1,0.4f,0.5f,0.15f};
    const Scenery forest=build_scenery(map);
    int trees=0,understory=0,branches=0,clutter=0;
    for(const auto& object:forest.objects) {
        if(tree(object.kind))++trees;else ++understory;
        if(object.kind==SceneryKind::Branch)++branches;
        if(object.kind==SceneryKind::Branch || object.kind==SceneryKind::Bush || object.kind==SceneryKind::Pebble)++clutter;
    }
    std::printf("full forest: %d trees, %d understory, %d fallen branches\n",trees,understory,branches);
    CHECK(trees>65536*0.13 && trees<65536*0.17);
    CHECK(understory>65536*0.20 && understory<65536*0.40);
    CHECK(branches>200 && branches<1200);
    CHECK(clutter<65536*0.065); // physical debris stays occasional even in dense woods
}

void ecological_identity() {
    TileMap map=plain();
    for(i32 y=0;y<map.biomes.height;++y)for(i32 x=0;x<map.biomes.width;++x) {
        auto& cell=map.biomes.cells[static_cast<size_t>(y*map.biomes.width+x)];
        if(x<map.biomes.width/2 && y<map.biomes.height/2)cell={1,0,0.4f,0};
        else if(y<map.biomes.height/2)cell={0,1,0,0};
        else if(x<map.biomes.width/2)cell={0,0,1,0};
        else cell={0,0,0,1};
    }
    const Scenery scene=build_scenery(map);
    int counts[4][15]{};
    for(const auto& object:scene.objects) {
        const i32 quadrant=(object.position.x>=128?1:0)+(object.position.y>=128?2:0);
        ++counts[quadrant][static_cast<i32>(object.kind)];
    }
    const i32 flower=static_cast<i32>(SceneryKind::Flower),bush=static_cast<i32>(SceneryKind::Bush),pebble=static_cast<i32>(SceneryKind::Pebble);
    CHECK(counts[1][flower]>counts[2][flower]*5);
    CHECK(counts[2][bush]>counts[1][bush]*5);
    CHECK(counts[3][pebble]>counts[1][pebble]*5);
    std::printf("biome identities: %d meadow flowers, %d thicket bushes, %d scree pebbles\n",counts[1][flower],counts[2][bush],counts[3][pebble]);
}

void generated_ecology(const char* output) {
    JobSystem jobs(4);
    for(u64 seed:{7u,42u,1234u}) {
        MapGenParams params;params.seed=seed;params.size=512;
        const TileMap map=generate_map(params,jobs);
        CHECK(!map.biomes.empty());
        double near_change=0,far_change=0;
        for(i32 y=8;y<map.height-40;y+=3)for(i32 x=8;x<map.width-40;x+=3) {
            const auto a=map.biomes.sample({static_cast<f32>(x),static_cast<f32>(y)});
            near_change+=std::abs(a.woodland-map.biomes.sample({static_cast<f32>(x+1),static_cast<f32>(y)}).woodland);
            far_change+=std::abs(a.woodland-map.biomes.sample({static_cast<f32>(x+32),static_cast<f32>(y)}).woodland);
        }
        CHECK(far_change>near_change*5);
        for(const auto& cell:map.biomes.cells) {
            CHECK(std::isfinite(cell.woodland+cell.meadow+cell.scrub+cell.rock));
            CHECK(cell.woodland>=0 && cell.woodland<=1 && cell.meadow>=0 && cell.meadow<=1);
            CHECK(cell.scrub>=0 && cell.scrub<=1 && cell.rock>=0 && cell.rock<=1);
        }
        int trees=0;
        for(const auto& object:map.scenery.objects)if(tree(object.kind)) {
            ++trees;
            CHECK(map.resource[static_cast<size_t>(object.tile)]==Resource::None);
            CHECK(!(map.flags[static_cast<size_t>(object.tile)]&TileFlag::Ramp));
            CHECK(length(object.position-Vec2{static_cast<f32>(map.start.x),static_cast<f32>(map.start.y)})>=14.0f);
        }
        std::printf("seed %llu: %zu objects, %d trees, %.1f%% reachable, %.1f ms\n",static_cast<unsigned long long>(seed),map.scenery.objects.size(),trees,
            100.0*map.stats.reachable_tiles/map.stats.walkable_tiles,map.stats.gen_ms);
        CHECK(map.stats.reachable_tiles>=map.stats.walkable_tiles*0.9);
        if(output && seed==7) {
            Image image(map.width,map.height);
            for(i32 y=0;y<map.height;++y)for(i32 x=0;x<map.width;++x) {
                const auto biome=map.biomes.sample({static_cast<f32>(x),static_cast<f32>(y)});
                image.at(x,y)={static_cast<u8>(biome.rock*255),static_cast<u8>(biome.woodland*255),static_cast<u8>(biome.meadow*255),255};
            }
            std::filesystem::create_directories(output);
            CHECK(write_png((std::filesystem::path(output)/"biome-density.png").string().c_str(),image));
        }
    }
    MapGenParams treeless;treeless.size=128;treeless.resolution=1;treeless.forest=0;
    const TileMap bare=generate_map(treeless,jobs);
    for(const auto& object:bare.scenery.objects)CHECK(!tree(object.kind));
}
}

int main(int argc,char** argv) {
    density_and_spacing();ecological_identity();generated_ecology(argc>1?argv[1]:nullptr);
    std::printf("biome distribution checks: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
