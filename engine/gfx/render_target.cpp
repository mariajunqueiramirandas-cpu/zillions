#include "gfx/render_target.h"

#include <algorithm>

#include "gfx/gl.h"

namespace zl {

RenderTarget::~RenderTarget() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
}

void RenderTarget::create(i32 width, i32 height) {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
    width_ = width;
    height_ = height;

    glGenTextures(1, &color_);
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
    ZL_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "render target is incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderTarget::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

void RenderTarget::bind_default(i32 width, i32 height) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
}

Image RenderTarget::read_pixels() const {
    Image image(width_, height_);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, image.bytes());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    // OpenGL returns the bottom row first.
    for (i32 y = 0; y < height_ / 2; ++y) {
        std::swap_ranges(image.pixels.begin() + y * width_, image.pixels.begin() + (y + 1) * width_, image.pixels.begin() + (height_ - 1 - y) * width_);
    }
    for (Color& c : image.pixels) c.a = 255;
    return image;
}

}  // namespace zl
