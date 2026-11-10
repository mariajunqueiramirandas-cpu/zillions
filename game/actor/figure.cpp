#include "actor/figure.h"

#include <algorithm>
#include <cmath>

namespace zl {

f32 round_cone_distance(Vec3 p, Vec3 a, Vec3 b, f32 ra, f32 rb) {
    const Vec3 ba = b - a, pa = p - a;
    const f32 h = dot(ba, ba), dr = ra - rb;
    f32 t;
    if (h <= dr * dr + 1.0e-12f) {
        // One end swallows the other: the shape is just the larger sphere.
        t = dr > 0.0f ? 0.0f : 1.0f;
    } else {
        // The surface point nearest to `p` belongs to the sphere whose centre
        // lies along the axis, shifted from the foot of the perpendicular
        // towards the wide end by the slope of the cone.
        const f32 along = dot(pa, ba) / h;
        const f32 across = length(pa - ba * along) / std::sqrt(h);
        t = clamp(along - across * dr / std::sqrt(h - dr * dr), 0.0f, 1.0f);
    }
    return length(p - (a + ba * t)) - lerp(ra, rb, t);
}

bool figure_screen_bounds(const Figure& figure, Vec2& lo, Vec2& hi) {
    if (figure.parts.empty()) return false;
    lo = {1.0e9f, 1.0e9f};
    hi = {-1.0e9f, -1.0e9f};
    for (const FigurePart& part : figure.parts) {
        const Vec2 a = figure_to_screen(part.a), b = figure_to_screen(part.b);
        lo.x = std::min({lo.x, a.x - part.ra, b.x - part.rb});
        lo.y = std::min({lo.y, a.y - part.ra, b.y - part.rb});
        hi.x = std::max({hi.x, a.x + part.ra, b.x + part.rb});
        hi.y = std::max({hi.y, a.y + part.ra, b.y + part.rb});
    }
    return true;
}

}  // namespace zl
