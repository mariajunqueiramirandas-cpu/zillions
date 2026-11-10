// Paints figures (see actor/figure.h). A figure has no texture: each is one
// quad, and for every pixel of it the fragment shader finds which of the
// figure's rounded cones is nearest the viewer, lights it in three or four
// values and gives its shadow side a dark edge, so the drawing is as sharp at
// 250 pixels per tile as at 16. All figures go out in a single draw call.
#pragma once

#include <vector>

#include "actor/figure.h"
#include "core/base.h"
#include "gfx/camera2d.h"
#include "gfx/shader.h"

namespace zl {

struct FigureDraw {
    const Figure* figure = nullptr;
    Vec2 position{0.0f, 0.0f};  // where it stands on the map, in tiles
    f32 time = 0.0f;            // seconds; moves the ripples round a wading figure
    f32 marker = 0.0f;          // 0..1: a ring on the ground to find the figure by from afar
};

class FigureRenderer {
public:
    FigureRenderer() = default;
    ~FigureRenderer();
    FigureRenderer(const FigureRenderer&) = delete;
    FigureRenderer& operator=(const FigureRenderer&) = delete;

    void init();

    // Draws the figures in the order given, so pass them north to south.
    void draw(const FigureDraw* figures, u32 count, const Camera2D& camera);

private:
    Shader shader_;
    i32 u_view_ = -1, u_pixel_ = -1;
    u32 vao_ = 0, vbo_ = 0, parts_ = 0;
    u32 instance_capacity_ = 0;
    i32 rows_ = 0;  // figures the parts texture has room for
    std::vector<f32> texels_;
};

}  // namespace zl
