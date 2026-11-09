// GPU textures.
#pragma once

#include <vector>

#include "core/base.h"
#include "core/image.h"

namespace zl {

enum class TextureFilter : u8 {
    Nearest,    // exact texel lookups (data textures)
    Linear,     // smooth, no mipmaps
    Trilinear,  // smooth with a generated mip chain
};

enum class TextureWrap : u8 { Clamp, Repeat };

class Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // 2D RGBA8 texture.
    void create(const Image& image, TextureFilter filter, TextureWrap wrap);
    // 2D array texture; all layers must have the same size.
    void create_array(const std::vector<Image>& layers, TextureFilter filter, TextureWrap wrap);

    void bind(u32 unit) const;
    u32 id() const { return id_; }
    i32 width() const { return width_; }
    i32 height() const { return height_; }

private:
    void reset(u32 target);

    u32 id_ = 0;
    u32 target_ = 0;
    i32 width_ = 0;
    i32 height_ = 0;
};

}  // namespace zl
