// Run with a display. Exercises the real model shader, skinning upload, depth,
// waterline clipping, tool draw and marker rather than a CPU approximation.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
void texture_orientation(RenderTarget& target, const IsometricCamera& camera) {
    // Constant UV (.25,.25), red-top/blue-bottom PNG. A second V flip
    // samples blue instead of red. Exercise decoding, upload and the shader.
    const auto root = std::filesystem::temp_directory_path() /
        ("zl-model-uv-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "models");
    {
        std::ofstream file(root / "models/plague_character.glb");
        file << R"({"asset":{"version":"2.0"},
          "buffers":[{"byteLength":60,"uri":"data:application/octet-stream;base64,mpkZv83MzD0AAAAAAACAPgAAgD6amRk/zczMPQAAAAAAAIA+AACAPgAAAADNzMw/AAAAAAAAgD4AAIA+"}],
          "bufferViews":[{"buffer":0,"byteLength":60,"byteStride":20}],
          "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                       {"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC2"}],
          "images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAAIElEQVR4nGP8z0AaYCJR/agGogAjAwNpMTEI/TAcNAAAwjACHXGKZsUAAAAASUVORK5CYII="}],
          "textures":[{"source":0}],
          "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":1}}],
          "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],
          "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    }
    ModelRenderer fixture;
    fixture.init(root.string().c_str());
    std::filesystem::remove_all(root);
    CHECK(fixture.ready());
    PlaguePose pose;
    pose.valid = true;
    pose.bones.push_back(Mat4::identity());
    PlagueDraw item;
    item.heading = kPi * 0.5f;
    target.bind();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    fixture.draw(camera, nullptr, 1.0f, item, pose);
    const Image image = target.read_pixels();
    size_t red = 0, blue = 0;
    for (const Color pixel : image.pixels) {
        if (int(pixel.r) > int(pixel.b) + 80) ++red;
        if (int(pixel.b) > int(pixel.r) + 80) ++blue;
    }
    CHECK(red > 1000);
    CHECK(blue == 0);
}
}  // namespace

int main(int argc, char** argv) {
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
    CHECK(models.model()->materials[0].base_color.width == 2048);
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

    item.marker = 0.0f;
    item.show_tool = false;
    const Image bare_hands = render();
    item.show_tool = true;
    const Image held_axe = render();
    CHECK(different(bare_hands, held_axe) > 50);

    // Optional review sheets from the actual shader. Rows sample the action
    // cycle; columns are the four camera rotations.
    if (argc == 3 && std::string(argv[1]) == "--captures") {
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        camera.center_height = 0.8f;
        for (const HumanTool tool : {HumanTool::Axe, HumanTool::Pickaxe}) {
            Image sheet(kSize * 4, kSize * 4);
            const f32 phases[] = {0.0f, 0.3f, 0.62f, 0.9f};
            for (i32 row = 0; row < 4; ++row) {
                motion = HumanMotion{};
                motion.tool = tool;
                motion.work = 1.0f;
                motion.work_phase = phases[row];
                animator.pose(motion);
                item.tool = tool;
                for (i32 column = 0; column < 4; ++column) {
                    camera.rotation = column;
                    const Image frame = render();
                    CHECK(occupied(frame) > 1000);
                    for (i32 y = 0; y < kSize; ++y)
                        for (i32 x = 0; x < kSize; ++x)
                            sheet.at(column * kSize + x, row * kSize + y) = frame.at(x, y);
                }
            }
            const auto path = directory / (tool == HumanTool::Axe ? "plague-axe-review.png" : "plague-pickaxe-review.png");
            CHECK(write_png(path.string().c_str(), sheet));
        }
        camera.rotation = 0;
    }
    texture_orientation(target, camera);

    std::printf("model render checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
