#include "world/map_preview.h"

#include <cmath>

namespace zl {

Color preview_color(const TileMap& map, i32 x, i32 y) {
    const size_t i = static_cast<size_t>(map.index(x, y));
    if (x == map.start.x && y == map.start.y) return {255, 255, 255, 255};
    switch (map.resource[i]) {
        case Resource::Stone: return {196, 196, 204, 255};
        case Resource::Iron: return {186, 92, 52, 255};
        case Resource::Gold: return {246, 204, 60, 255};
        default: break;
    }
    const f32 shade = 0.8f + 0.4f * map.elevation[i];
    const auto lit = [shade](f32 r, f32 g, f32 b) {
        return Color{static_cast<u8>(clamp(r * shade, 0.0f, 255.0f)), static_cast<u8>(clamp(g * shade, 0.0f, 255.0f)),
                     static_cast<u8>(clamp(b * shade, 0.0f, 255.0f)), 255};
    };
    switch (map.terrain[i]) {
        case Terrain::Water: {
            const f32 depth = saturate(static_cast<f32>(map.shore_dist[i]) / 96.0f);
            return {static_cast<u8>(lerp(64.0f, 22.0f, depth)), static_cast<u8>(lerp(140.0f, 70.0f, depth)), static_cast<u8>(lerp(190.0f, 130.0f, depth)), 255};
        }
        case Terrain::Shallows: return {122, 186, 196, 255};
        case Terrain::Sand: return lit(214, 196, 140);
        case Terrain::Grass: return lit(104, 150, 66);
        case Terrain::Dirt: return lit(140, 118, 84);
        case Terrain::Forest: return lit(44, 92, 46);
        case Terrain::Mountain: return lit(112, 108, 104);
        case Terrain::Count: break;
    }
    return {255, 0, 255, 255};
}

Image render_map_preview(const TileMap& map, i32 pixels_per_tile) {
    const i32 s = pixels_per_tile < 1 ? 1 : pixels_per_tile;
    Image image(map.width * s, map.height * s);
    for (i32 y = 0; y < map.height; ++y) {
        for (i32 x = 0; x < map.width; ++x) {
            const Color c = preview_color(map, x, y);
            for (i32 py = 0; py < s; ++py) {
                for (i32 px = 0; px < s; ++px) image.at(x * s + px, y * s + py) = c;
            }
        }
    }
    return image;
}

}  // namespace zl
