// A figure is a character described entirely in code: a list of rounded cones
// in three dimensions, each with a colour and a material. There is no image
// behind it. The rig that poses the cones lives in actor/human.*; the shader
// that paints them is in render/figure_renderer.*. Everything here is headless.
#pragma once

#include <vector>

#include "core/base.h"
#include "core/image.h"
#include "math/vec.h"

namespace zl {

// The most parts one figure may have; the renderer stores a figure as one row
// of a data texture this many parts wide.
inline constexpr i32 kMaxFigureParts = 128;

// How a part takes the light. The shader paints each in three or four values.
enum class FigureMaterial : u8 {
    Cloth,    // matte, broad brushwork
    Skin,     // smooth, warm in shadow
    Leather,  // dark in shadow, a dull sheen
    Hair,     // streaked along its length
    Metal,    // hard highlight
    Count
};

namespace FigurePartFlag {
enum : u8 {
    FlatCaps = 1 << 0,  // cut square at both ends instead of rounded: bands, hems, cuffs
};
}

// One rounded cone: every point within `ra` of `a`, within `rb` of `b`, and
// everything between. Positions are in figure space: tiles, x east, y south,
// z up, with the origin on the ground where the figure stands.
struct FigurePart {
    Vec3 a, b;
    f32 ra = 0.0f, rb = 0.0f;
    Color color;
    FigureMaterial material = FigureMaterial::Cloth;
    u8 flags = 0;
    // Parts of one surface share a group. Within a group they melt into each
    // other; between groups the nearer part keeps a dark edge on its shadow side.
    u8 group = 0;
    f32 roundness = 1.0f;  // 1: lit as a full cylinder; less flattens the shading
};

struct Figure {
    std::vector<FigurePart> parts;
    // Parts below this height (in tiles) are under water and are not drawn.
    f32 waterline = 0.0f;
};

// The map is drawn in plan and everything standing on it is drawn as seen from
// the south at about 40 degrees above the horizon. These are the sine and
// cosine of that angle: how far a step south and a step up move a point on
// the screen.
inline constexpr f32 kFigureDepthScale = 0.64f;
inline constexpr f32 kFigureUpScale = 0.77f;

// Screen position of a figure-space point, in tiles relative to the ground
// point, with +y down the screen.
constexpr Vec2 figure_to_screen(Vec3 p) { return {p.x, p.y * kFigureDepthScale - p.z * kFigureUpScale}; }
// Distance towards the viewer; of two overlapping parts the greater is in front.
constexpr f32 figure_depth(Vec3 p) { return p.y * kFigureUpScale + p.z * kFigureDepthScale; }

// Signed distance from `p` to a rounded cone: negative inside.
f32 round_cone_distance(Vec3 p, Vec3 a, Vec3 b, f32 ra, f32 rb);

// The rectangle the figure's body covers on screen, in tiles relative to its
// ground point. Returns false for an empty figure.
bool figure_screen_bounds(const Figure& figure, Vec2& lo, Vec2& hi);

}  // namespace zl
