#include "world/biome.h"

#include <algorithm>
#include <cmath>

namespace zl {
namespace {
BiomeDensity blend(BiomeDensity a, BiomeDensity b, f32 t) {
    return {lerp(a.woodland,b.woodland,t),lerp(a.meadow,b.meadow,t),
            lerp(a.scrub,b.scrub,t),lerp(a.rock,b.rock,t)};
}
}

void BiomeField::create(i32 map_width, i32 map_height) {
    width = (map_width + kSpacing - 1) / kSpacing + 1;
    height = (map_height + kSpacing - 1) / kSpacing + 1;
    cells.assign(static_cast<size_t>(width * height), {});
}

BiomeDensity BiomeField::sample(Vec2 position) const {
    if (empty()) return {};
    const f32 px = clamp(position.x / kSpacing, 0.0f, static_cast<f32>(width - 1));
    const f32 py = clamp(position.y / kSpacing, 0.0f, static_cast<f32>(height - 1));
    const i32 x = static_cast<i32>(px), y = static_cast<i32>(py);
    const i32 right = std::min(x + 1, width - 1), bottom = std::min(y + 1, height - 1);
    const f32 fx=px-static_cast<f32>(x),fy=py-static_cast<f32>(y);
    return blend(blend(cells[static_cast<size_t>(y*width+x)],cells[static_cast<size_t>(y*width+right)],fx),
                 blend(cells[static_cast<size_t>(bottom*width+x)],cells[static_cast<size_t>(bottom*width+right)],fx),fy);
}

void BiomeField::add_patch(Vec2 center, f32 radius, BiomeDensity density) {
    if (empty()) return;
    const f32 reach = radius + 7.0f;
    const i32 left = clamp(static_cast<i32>((center.x-reach)/kSpacing),0,width-1);
    const i32 right = clamp(static_cast<i32>(std::ceil((center.x+reach)/kSpacing)),0,width-1);
    const i32 top = clamp(static_cast<i32>((center.y-reach)/kSpacing),0,height-1);
    const i32 bottom = clamp(static_cast<i32>(std::ceil((center.y+reach)/kSpacing)),0,height-1);
    for(i32 y=top;y<=bottom;++y)for(i32 x=left;x<=right;++x) {
        const f32 weight=1.0f-smoothstep(radius*0.35f,reach,length(Vec2{static_cast<f32>(x*kSpacing),static_cast<f32>(y*kSpacing)}-center));
        auto& cell=cells[static_cast<size_t>(y*width+x)];
        cell.woodland=std::max(cell.woodland,density.woodland*weight);
        cell.meadow=std::max(cell.meadow,density.meadow*weight);
        cell.scrub=std::max(cell.scrub,density.scrub*weight);
        cell.rock=std::max(cell.rock,density.rock*weight);
    }
}

} // namespace zl
