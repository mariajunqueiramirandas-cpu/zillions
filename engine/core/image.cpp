#include "core/image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image_write.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#include <stb_image.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cstring>

namespace zl {

static_assert(sizeof(Color) == 4, "Color must be tightly packed RGBA8");

bool write_png(const char* path, const Image& image) {
    if (image.width <= 0 || image.height <= 0) return false;
    return stbi_write_png(path, image.width, image.height, 4, image.bytes(), image.width * 4) != 0;
}

bool read_png(const char* path, Image& image) {
    int width = 0, height = 0, channels = 0;
    stbi_uc* data = stbi_load(path, &width, &height, &channels, 4);
    if (!data) return false;
    image = Image(width, height);
    std::memcpy(image.bytes(), data, image.pixels.size() * sizeof(Color));
    stbi_image_free(data);
    return true;
}

}  // namespace zl
