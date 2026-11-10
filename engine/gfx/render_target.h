// Off-screen colour and depth buffers, used for screenshots at arbitrary resolution.
#pragma once

#include "core/base.h"
#include "core/image.h"

namespace zl {

class RenderTarget {
public:
    RenderTarget() = default;
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    void create(i32 width, i32 height);
    void bind() const;             // also sets the viewport
    static void bind_default(i32 width, i32 height);

    // Copies the colour buffer to CPU memory with the top row first.
    Image read_pixels() const;

    i32 width() const { return width_; }
    i32 height() const { return height_; }

private:
    u32 fbo_ = 0;
    u32 color_ = 0;
    u32 depth_ = 0;
    i32 width_ = 0;
    i32 height_ = 0;
};

}  // namespace zl
