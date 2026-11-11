#include "gfx/texture.h"

#include "gfx/gl.h"

namespace zl {

namespace {

void set_sampling(GLenum target, TextureFilter filter, TextureWrap wrap) {
    const GLint min_filter = filter == TextureFilter::Nearest ? GL_NEAREST : (filter == TextureFilter::Linear ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
    const GLint mag_filter = filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
    const GLint wrap_mode = wrap == TextureWrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, mag_filter);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap_mode);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap_mode);
    if (filter == TextureFilter::Trilinear) glGenerateMipmap(target);
}

}  // namespace

Texture::~Texture() {
    if (id_) glDeleteTextures(1, &id_);
}

Texture::Texture(Texture&& other) noexcept : id_(other.id_), target_(other.target_), width_(other.width_), height_(other.height_) {
    other.id_ = 0;
    other.target_ = 0;
    other.width_ = other.height_ = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (id_) glDeleteTextures(1, &id_);
        id_ = other.id_;
        target_ = other.target_;
        width_ = other.width_;
        height_ = other.height_;
        other.id_ = 0;
        other.target_ = 0;
        other.width_ = other.height_ = 0;
    }
    return *this;
}

void Texture::reset(u32 target) {
    if (id_) glDeleteTextures(1, &id_);
    glGenTextures(1, &id_);
    target_ = target;
    glBindTexture(target_, id_);
}

void Texture::create(const Image& image, TextureFilter filter, TextureWrap wrap, i32 max_mip_level) {
    reset(GL_TEXTURE_2D);
    width_ = image.width;
    height_ = image.height;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.bytes());
    if (max_mip_level >= 0) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max_mip_level);
    set_sampling(GL_TEXTURE_2D, filter, wrap);
}

void Texture::create_float(i32 width, i32 height, const f32* values) {
    reset(GL_TEXTURE_2D);
    width_ = width;
    height_ = height;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, values);
    set_sampling(GL_TEXTURE_2D, TextureFilter::Linear, TextureWrap::Clamp);
}

void Texture::create_array(const std::vector<Image>& layers, TextureFilter filter, TextureWrap wrap) {
    ZL_CHECK(!layers.empty(), "texture array needs at least one layer");
    reset(GL_TEXTURE_2D_ARRAY);
    width_ = layers[0].width;
    height_ = layers[0].height;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, width_, height_, static_cast<GLsizei>(layers.size()), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (size_t i = 0; i < layers.size(); ++i) {
        ZL_CHECK(layers[i].width == width_ && layers[i].height == height_, "texture array layers must share one size");
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(i), width_, height_, 1, GL_RGBA, GL_UNSIGNED_BYTE, layers[i].bytes());
    }
    set_sampling(GL_TEXTURE_2D_ARRAY, filter, wrap);
}

void Texture::bind(u32 unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(target_, id_);
}

}  // namespace zl
