#include "gfx/render_target.h"

#include <algorithm>

#include "gfx/gl.h"

namespace zl {

RenderTarget::~RenderTarget() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
    if (multisample_fbo_) glDeleteFramebuffers(1, &multisample_fbo_);
    if (multisample_color_) glDeleteRenderbuffers(1, &multisample_color_);
    if (multisample_depth_) glDeleteRenderbuffers(1, &multisample_depth_);
}

void RenderTarget::create(i32 width, i32 height, i32 samples) {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
    if (multisample_fbo_) glDeleteFramebuffers(1, &multisample_fbo_);
    if (multisample_color_) glDeleteRenderbuffers(1, &multisample_color_);
    if (multisample_depth_) glDeleteRenderbuffers(1, &multisample_depth_);
    multisample_fbo_ = multisample_color_ = multisample_depth_ = 0;
    width_ = width;
    height_ = height;
    GLint maximum = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maximum);
    samples_ = std::clamp(samples, 1, std::max(maximum, 1));

    glGenTextures(1, &color_);
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
    ZL_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "render target is incomplete");
    if (samples_ > 1) {
        glGenFramebuffers(1, &multisample_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, multisample_fbo_);
        glGenRenderbuffers(1, &multisample_color_);
        glBindRenderbuffer(GL_RENDERBUFFER, multisample_color_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, multisample_color_);
        glGenRenderbuffers(1, &multisample_depth_);
        glBindRenderbuffer(GL_RENDERBUFFER, multisample_depth_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, multisample_depth_);
        ZL_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "multisample target is incomplete");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderTarget::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, multisample_fbo_ ? multisample_fbo_ : fbo_);
    glEnable(GL_MULTISAMPLE);
    glViewport(0, 0, width_, height_);
}

void RenderTarget::resolve() const {
    if (!multisample_fbo_) return;
    GLint draw = 0, read = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, multisample_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<u32>(draw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<u32>(read));
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

void RenderTarget::bind_read() const {
    resolve();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
}

void RenderTarget::present() const {
    resolve();
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

void RenderTarget::bind_default(i32 width, i32 height) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
}

Image RenderTarget::read_pixels() const {
    Image image(width_, height_);
    GLint read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    bind_read();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, image.bytes());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<u32>(read));
    // OpenGL returns the bottom row first.
    for (i32 y = 0; y < height_ / 2; ++y) {
        std::swap_ranges(image.pixels.begin() + y * width_, image.pixels.begin() + (y + 1) * width_, image.pixels.begin() + (height_ - 1 - y) * width_);
    }
    for (Color& c : image.pixels) c.a = 255;
    return image;
}

}  // namespace zl
