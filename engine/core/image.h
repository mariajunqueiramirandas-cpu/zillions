// CPU-side RGBA8 image, used for procedural art, screenshots and previews.
#pragma once

#include <vector>

#include "core/base.h"

namespace zl {

struct Color {
    u8 r = 0, g = 0, b = 0, a = 255;
};

struct Image {
    i32 width = 0;
    i32 height = 0;
    std::vector<Color> pixels;

    Image() = default;
    Image(i32 w, i32 h, Color fill = {0, 0, 0, 0}) : width(w), height(h), pixels(static_cast<size_t>(w) * static_cast<size_t>(h), fill) {}

    Color& at(i32 x, i32 y) { return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]; }
    const Color& at(i32 x, i32 y) const { return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]; }
    const u8* bytes() const { return reinterpret_cast<const u8*>(pixels.data()); }
    u8* bytes() { return reinterpret_cast<u8*>(pixels.data()); }
};

// Writes an RGBA PNG. Returns false if the file could not be written.
bool write_png(const char* path, const Image& image);

}  // namespace zl
