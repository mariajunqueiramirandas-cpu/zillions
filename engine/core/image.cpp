#include "core/image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace zl {

static_assert(sizeof(Color) == 4, "Color must be tightly packed RGBA8");

bool write_png(const char* path, const Image& image) {
    if (image.width <= 0 || image.height <= 0) return false;
    return stbi_write_png(path, image.width, image.height, 4, image.bytes(), image.width * 4) != 0;
}

}  // namespace zl
