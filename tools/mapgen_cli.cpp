// Headless map generator.
//   zl_mapgen [--seed N] [--size N] [--out preview.png] [--scale N] [--bench]
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/clock.h"
#include "core/jobs.h"
#include "world/map_preview.h"
#include "world/mapgen.h"

using namespace zl;

namespace {

void print_stats(const TileMap& map) {
    const MapStats& s = map.stats;
    const f64 total = static_cast<f64>(map.tile_count());
    std::printf("seed %llu, %dx%d, generated in %.2f ms\n", static_cast<unsigned long long>(map.seed), map.width, map.height, s.gen_ms);
    for (int t = 0; t < kTerrainCount; ++t) {
        std::printf("  %-9s %6d  %5.1f%%\n", terrain_name(static_cast<Terrain>(t)), s.terrain_tiles[t], 100.0 * s.terrain_tiles[t] / total);
    }
    std::printf("  deposits: %d stone, %d iron, %d gold\n", s.deposits[1], s.deposits[2], s.deposits[3]);
    std::printf("  height: %d / %d / %d walkable tiles on levels 0 / 1 / 2; %d cliff edges, %d ramps\n", s.level_tiles[0], s.level_tiles[1],
                s.level_tiles[2], s.cliff_edges, s.ramps);
    std::printf("  rivers: %d tiles, %d fords; %d tiles carved for connectivity\n", s.river_tiles, s.fords, s.carved_tiles);
    std::printf("  reachable: %d of %d walkable tiles (%.1f%%), %d map-edge entry tiles\n", s.reachable_tiles, s.walkable_tiles,
                s.walkable_tiles ? 100.0 * s.reachable_tiles / s.walkable_tiles : 0.0, s.reachable_border_tiles);
    std::printf("  buildable tiles near start: %d\n", s.buildable_near_start);
}

}  // namespace

int main(int argc, char** argv) {
    MapGenParams params;
    const char* out = nullptr;
    int scale = 4;
    bool bench = false;
    for (int i = 1; i < argc; ++i) {
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (!std::strcmp(argv[i], "--seed")) params.seed = std::strtoull(value(), nullptr, 10);
        else if (!std::strcmp(argv[i], "--size")) params.size = std::atoi(value());
        else if (!std::strcmp(argv[i], "--out")) out = value();
        else if (!std::strcmp(argv[i], "--scale")) scale = std::atoi(value());
        else if (!std::strcmp(argv[i], "--bench")) bench = true;
        else {
            std::fprintf(stderr, "usage: zl_mapgen [--seed N] [--size N] [--out preview.png] [--scale N] [--bench]\n");
            return 2;
        }
    }

    JobSystem jobs;
    if (bench) {
        std::printf("map generation, %d threads (best of 5)\n", jobs.thread_count());
        for (const i32 size : {128, 256, 512, 1024}) {
            f64 best = 1.0e9;
            MapGenParams p = params;
            p.size = size;
            for (int run = 0; run < 5; ++run) {
                p.seed = params.seed + static_cast<u64>(run);
                const TileMap map = generate_map(p, jobs);
                if (map.stats.gen_ms < best) best = map.stats.gen_ms;
            }
            std::printf("  %4dx%-4d  %8.2f ms  %6.1f M tiles/s\n", size, size, best, size * size / best / 1000.0);
            if (size == params.size) {
                const TileMap map = generate_map(p, jobs);
                for (const MapStats::Phase& phase : map.stats.phases) std::printf("      %-14s %7.2f ms\n", phase.name, phase.ms);
            }
        }
        return 0;
    }

    const TileMap map = generate_map(params, jobs);
    print_stats(map);
    if (out) {
        if (!write_png(out, render_map_preview(map, scale))) {
            std::fprintf(stderr, "could not write %s\n", out);
            return 1;
        }
        std::printf("wrote %s\n", out);
    }
    return 0;
}
