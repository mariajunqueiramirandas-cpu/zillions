// Run with a display. Exercises the real model shader, skinning upload, depth,
// waterline clipping, tool draw and marker rather than a CPU approximation.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "actor/plague.h"
#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "platform/window.h"
#include "render/model_renderer.h"

using namespace zl;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAILED line %d: %s\n", __LINE__, #condition); } } while (0)

constexpr i32 kSize = 320;
const Color kBackground{31, 33, 36, 255};

bool near_background(Color c) {
    return std::abs(int(c.r) - int(kBackground.r)) <= 3 && std::abs(int(c.g) - int(kBackground.g)) <= 3 &&
           std::abs(int(c.b) - int(kBackground.b)) <= 3;
}

size_t occupied(const Image& image) {
    size_t count = 0;
    for (const Color c : image.pixels) {
        if (!near_background(c)) ++count;
    }
    return count;
}

size_t different(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return a.pixels.size() + b.pixels.size();
    size_t count = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const Color x = a.pixels[i], y = b.pixels[i];
        if (std::abs(int(x.r) - int(y.r)) > 3 || std::abs(int(x.g) - int(y.g)) > 3 ||
            std::abs(int(x.b) - int(y.b)) > 3 || std::abs(int(x.a) - int(y.a)) > 3) ++count;
    }
    return count;
}
}  // namespace

int main() {
    WindowConfig config;
    config.visible = false;
    config.width = config.height = kSize;
    Window window(config);

    ModelRenderer models;
    models.init(ZL_ASSET_DIR);
    CHECK(models.ready());
    if (!models.ready()) {
        std::printf("model render checks: %d failures (model not loaded)\n", failures);
        return 1;
    }
    PlagueAnimator animator;
    CHECK(animator.init(models.model()));
    if (!animator.ready()) {
        std::printf("model render checks: %d failures (animator not ready)\n", failures);
        return 1;
    }

    RenderTarget target;
    target.create(kSize, kSize);
    IsometricCamera camera;
    camera.viewport = {kSize, kSize};
    camera.zoom = 128.0f;
    camera.center = {0.0f, 0.0f};
    camera.center_height = 0.0f;

    PlagueDraw item;
    item.position = {0.0f, 0.0f};
    item.ground_height = 0.0f;
    item.heading = kPi * 0.5f;

    const auto render = [&]() {
        target.bind();
        glClearColor(static_cast<f32>(kBackground.r) / 255.0f, static_cast<f32>(kBackground.g) / 255.0f,
                     static_cast<f32>(kBackground.b) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        models.draw(camera, nullptr, 1.0f, item, animator.current());
        models.draw_marker(camera, item);
        return target.read_pixels();
    };

    HumanMotion motion;
    motion.speed = 0.0f;
    motion.time = 0.0f;
    animator.pose(motion);
    const Image idle = render();
    CHECK(occupied(idle) > 1000);
    CHECK(near_background(idle.at(2, 2)));
    CHECK(models.draw_calls() >= 2);

    motion.time = 2.0f;
    animator.pose(motion);
    const Image idle_later = render();
    CHECK(different(idle, idle_later) > 100);

    motion.speed = 5.2f;
    motion.phase = 0.3f;
    animator.pose(motion);
    const Image run = render();
    CHECK(different(idle, run) > 100);

    item.tool = HumanTool::Axe;
    item.show_tool = true;
    motion.tool = HumanTool::Axe;
    motion.work = 1.0f;
    motion.work_phase = 0.6f;
    animator.pose(motion);
    const Image axe = render();
    CHECK(different(run, axe) > 100);
    CHECK(models.draw_calls() >= 3);

    item.wade = 1.0f;
    const Image wade = render();
    CHECK(different(axe, wade) > 100);
    CHECK(near_background(wade.at(kSize / 2, kSize - 6)));

    item.wade = 0.0f;
    item.marker = 1.0f;
    const Image marker = render();
    CHECK(different(wade, marker) > 100);

    Vec2 lo, hi;
    CHECK(models.reveal_bounds(camera, item, lo, hi));
    CHECK(hi.x > lo.x && hi.y > lo.y);

    std::printf("model render checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
