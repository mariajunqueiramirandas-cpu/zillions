// Run with a display or xvfb-run. These checks exercise the actual shaders,
// atlas composition and framebuffer state, rather than a CPU approximation.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "actor/human.h"
#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "platform/window.h"
#include "render/figure_renderer.h"

using namespace zl;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAILED line %d: %s\n", __LINE__, #condition); } } while (0)

Image read_rgba(i32 width, i32 height) {
    GLint draw = 0, read = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(draw));
    Image image(width, height);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.bytes());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read));
    for (i32 y = 0; y < height / 2; ++y) {
        std::swap_ranges(image.pixels.begin() + y * width, image.pixels.begin() + (y + 1) * width,
                        image.pixels.begin() + (height - y - 1) * width);
    }
    return image;
}

bool same_image(const Image& a, const Image& b, int tolerance = 2) {
    if (a.width != b.width || a.height != b.height) return false;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const Color x = a.pixels[i], y = b.pixels[i];
        if (std::abs(int(x.r) - int(y.r)) > tolerance || std::abs(int(x.g) - int(y.g)) > tolerance ||
            std::abs(int(x.b) - int(y.b)) > tolerance || std::abs(int(x.a) - int(y.a)) > tolerance) return false;
    }
    return true;
}

void check_premultiplied(const Image& image) {
    bool valid = true;
    size_t occupied = 0;
    for (Color c : image.pixels) {
        valid &= int(c.r) <= int(c.a) + 1 && int(c.g) <= int(c.a) + 1 && int(c.b) <= int(c.a) + 1;
        if (c.a > 10) ++occupied;
    }
    CHECK(valid);
    CHECK(occupied > 10 && occupied < image.pixels.size() / 2);
    CHECK(image.at(0, 0).a == 0 && image.at(image.width - 1, image.height - 1).a == 0);
}
}

int main() {
    WindowConfig config;
    config.visible = false;
    config.width = config.height = 256;
    Window window(config);
    FigureRenderer renderer;
    renderer.init();
    RenderTarget target;
    target.create(256, 256);
    Camera2D camera;
    camera.center = {0.0f, -0.5f};
    camera.viewport = {256, 256};
    camera.zoom = 128;
    Figure figure;
    HumanMotion motion;
    motion.time = 0.9f;
    build_human(hero_look(), motion, figure);
    const FigureDraw item{&figure, {0, 0}, 0.9f, 0};

    const auto draw = [&](const FigureDraw* items, u32 count) {
        target.bind();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); // Deliberately different read/draw targets.
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        GLint expected_draw = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &expected_draw);
        renderer.draw(items, count, camera);
        GLint actual_draw = 0, actual_read = -1, viewport[4]{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &actual_draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &actual_read);
        glGetIntegerv(GL_VIEWPORT, viewport);
        CHECK(actual_draw == expected_draw && actual_read == 0);
        CHECK(viewport[0] == 0 && viewport[1] == 0 && viewport[2] == 256 && viewport[3] == 256);
        CHECK(glGetError() == GL_NO_ERROR);
        return read_rgba(256, 256);
    };

    const Image painted = draw(&item, 1);
    CHECK(renderer.draw_calls() == 2);
    check_premultiplied(painted);
    int red_count = 0;
    f32 red_y = 0;
    for (int y = 0; y < painted.height; ++y) {
        for (int x = 0; x < painted.width; ++x) {
            const Color c = painted.at(x, y);
            if (c.r > 45 && int(c.r) > int(c.g) * 3 / 2 && int(c.r) > int(c.b) * 3 / 2) { ++red_count; red_y += static_cast<f32>(y); }
        }
    }
    CHECK(red_count > 20);
    CHECK(red_y / static_cast<f32>(std::max(red_count, 1)) < 128); // The hood is above the feet, not flipped.

    camera.zoom = 64;
    HumanLook other_look = hero_look();
    other_look.hood = {40, 100, 170, 255};
    other_look.beard = true;
    other_look.hair_style = HairStyle::Long;
    HumanMotion other_motion = motion;
    other_motion.speed = 3;
    other_motion.phase = 0.31f;
    other_motion.wade = 0.24f;
    Figure other;
    build_human(other_look, other_motion, other);
    const FigureDraw pair[] = {{&figure, {-1.0f, 0}, 0.9f, 0}, {&other, {1.0f, 0}, 0.9f, 0}, {&figure, {1000, 1000}, 0.9f, 0}};
    const Image batched = draw(pair, 3); // Grows the atlas and culls the third figure.
    check_premultiplied(batched);
    int red_left = 0, blue_right = 0;
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 256; ++x) {
            const Color c = batched.at(x, y);
            if (x < 128 && c.r > 45 && int(c.r) > int(c.g) * 3 / 2) ++red_left;
            if (x >= 128 && c.b > 45 && int(c.b) > int(c.r) * 3 / 2 && int(c.b) > int(c.g) * 3 / 2) ++blue_right;
        }
    }
    CHECK(red_left > 5 && blue_right > 5);
    CHECK(same_image(batched, draw(pair, 2)));
    camera.zoom = 128;
    CHECK(same_image(painted, draw(&item, 1))); // Reuse must clear old sprites and preserve framing.
    renderer.set_style(FigureStyle::Smooth);
    const Image smooth = draw(&item, 1);
    CHECK(renderer.draw_calls() == 1);
    CHECK(!same_image(painted, smooth, 10));
    renderer.set_style(FigureStyle::Painted);
    CHECK(same_image(painted, draw(&item, 1)));

    // Viewport offsets and clear colour belong to the caller too.
    target.bind();
    glViewport(7, 9, 180, 196);
    glClearColor(0.17f, 0.23f, 0.31f, 0.4f);
    renderer.draw(&item, 1, camera);
    GLint viewport[4]{};
    GLfloat clear[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    CHECK(viewport[0] == 7 && viewport[1] == 9 && viewport[2] == 180 && viewport[3] == 196);
    CHECK(std::fabs(clear[0] - 0.17f) < 1.0e-6f && std::fabs(clear[3] - 0.4f) < 1.0e-6f);

    camera.zoom = 6;
    camera.center = {0, 0};
    const FigureDraw marker{&figure, {0, 0}, 0, 1};
    const Image far = draw(&marker, 1);
    check_premultiplied(far);
    CHECK(far.at(141, 128).a > 50); // Thirteen-screen-pixel ring survives the texture pass.
    CHECK(far.at(153, 128).a == 0);
    draw(nullptr, 0);
    CHECK(renderer.draw_calls() == 0);
    CHECK(glGetError() == GL_NO_ERROR);
    std::printf("GPU figure checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
