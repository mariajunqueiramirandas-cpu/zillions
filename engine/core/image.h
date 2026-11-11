// CPU-side RGBA8 image, used for art, screenshots and previews.
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

// Reads a PNG as RGBA8 with straight alpha; images without an alpha channel
// come back opaque. Returns false, leaving `image` alone, if the file is
// missing or is not a PNG.
bool read_png(const char* path, Image& image);

// Decodes PNG or JPEG bytes (as embedded in a GLB) to RGBA8 with straight
// alpha. Returns false, leaving `image` alone, on malformed data.
bool decode_image(const u8* data, size_t size, Image& image);

}  // namespace zl
