// Paints the live procedural pose into a small transparent atlas, then draws
// it as a softly filtered sprite. Smooth mode draws the cones directly.
#pragma once

#include <vector>

#include "actor/figure.h"
#include "core/base.h"
#include "gfx/camera2d.h"
#include "gfx/shader.h"
#include "gfx/render_target.h"
#include "gfx/sprite_batch.h"

namespace zl {

enum class FigureStyle : u8 { Painted, Smooth };

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
    void set_style(FigureStyle style) { style_ = style; }
    FigureStyle style() const { return style_; }
    u32 draw_calls() const { return draw_calls_; }

    // Draws the figures in the order given, so pass them north to south.
    void draw(const FigureDraw* figures, u32 count, const Camera2D& camera);

private:
    Shader shader_;
    i32 u_view_ = -1, u_pixel_ = -1, u_painted_ = -1;
    u32 vao_ = 0, vbo_ = 0, parts_ = 0;
    u32 instance_capacity_ = 0;
    i32 rows_ = 0;  // figures the parts texture has room for
    std::vector<f32> texels_;
    FigureStyle style_ = FigureStyle::Painted;
    RenderTarget painted_;
    SpriteBatch sprites_;
    i32 max_texture_size_ = 0;
    u32 draw_calls_ = 0;
};

}  // namespace zl
