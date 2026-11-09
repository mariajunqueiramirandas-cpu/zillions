// Flat-colour rendering of a map on the CPU: one pixel block per tile. Needs no
// GPU, so it works on servers and in tests, and doubles as minimap source.
#pragma once

#include "core/image.h"
#include "world/tilemap.h"

namespace zl {

Color preview_color(const TileMap& map, i32 x, i32 y);
Image render_map_preview(const TileMap& map, i32 pixels_per_tile = 1);

}  // namespace zl
