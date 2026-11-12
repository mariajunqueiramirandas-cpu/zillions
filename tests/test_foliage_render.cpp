// Real GPU regressions for plant shadows, multisample coverage and presentation.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/jobs.h"
#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "gfx/shader.h"
#include "platform/window.h"
#include "render/map_renderer.h"

using namespace zl;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

bool same_image(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return false;
    for (size_t p = 0; p < a.pixels.size(); ++p) {
        const Color x = a.pixels[p], y = b.pixels[p];
        if (x.r != y.r || x.g != y.g || x.b != y.b) return false;
    }
    return true;
}

void check_multisampling() {
    Shader shader;
    shader.compile("thin geometry", R"(#version 330 core
        uniform float uOffset;
        void main() {
            const vec2 p[3]=vec2[3](vec2(-0.91,-0.71),vec2(0.83,0.61),vec2(0.83,0.624));
            gl_Position=vec4(p[gl_VertexID]+vec2(uOffset*2.0/128.0,0),0,1);
        })", R"(#version 330 core
        out vec4 color;
        void main(){color=vec4(1);})");
    u32 vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    RenderTarget single, multi;
    single.create(128, 128);
    multi.create(128, 128, 4);
    CHECK(multi.samples() >= 4);
    const auto render = [&](RenderTarget& target, f32 offset) {
        target.bind();
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        shader.bind(); glUniform1f(shader.uniform("uOffset"), offset);
        glBindVertexArray(vao); glDrawArrays(GL_TRIANGLES, 0, 3);
        return target.read_pixels();
    };
    const Image hard = render(single, 0), smooth = render(multi, 0);
    i32 partial_single = 0, partial_multi = 0;
    for (size_t p = 0; p < hard.pixels.size(); ++p) {
        if (hard.pixels[p].r > 0 && hard.pixels[p].r < 255) ++partial_single;
        if (smooth.pixels[p].r > 0 && smooth.pixels[p].r < 255) ++partial_multi;
    }
    CHECK(partial_single == 0);
    CHECK(partial_multi > 100);
    std::printf("thin edges: %d fractional pixels at 1x, %d at 4x\n", partial_single, partial_multi);

    // Move the same subpixel geometry in sixteen increments. AA should retain
    // its coverage instead of alternating between vanished and solid pixels.
    double minimum[2]{1e30, 1e30}, maximum[2]{};
    for (i32 mode = 0; mode < 2; ++mode) for (i32 step = 0; step < 16; ++step) {
        const Image image = render(mode ? multi : single, static_cast<f32>(step) / 16);
        double energy = 0;
        for (Color pixel : image.pixels) energy += pixel.r;
        minimum[mode] = std::min(minimum[mode], energy);
        maximum[mode] = std::max(maximum[mode], energy);
    }
    const double variation_single = (maximum[0] - minimum[0]) / maximum[0];
    const double variation_multi = (maximum[1] - minimum[1]) / maximum[1];
    std::printf("subpixel coverage variation: %.3f at 1x, %.3f at 4x\n", variation_single, variation_multi);
    CHECK(variation_multi < variation_single);
    CHECK(minimum[1] > maximum[1] * 0.90);

    const Image expected = render(multi, 0.3f);
    GLint draw = 0, read = 0, viewport[4]{}, restored[4]{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 1, 1);
    CHECK(same_image(expected, multi.read_pixels()));
    CHECK(multi.color_texture() != 0);
    GLint actual = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &actual); CHECK(actual == draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &actual); CHECK(actual == read);
    glGetIntegerv(GL_VIEWPORT, restored);
    CHECK(std::equal(viewport, viewport + 4, restored));
    CHECK(glIsEnabled(GL_SCISSOR_TEST));
    multi.present();
    CHECK(glIsEnabled(GL_SCISSOR_TEST));
    Image window(128, 128);
    glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, window.bytes());
    for (i32 y = 0; y < 64; ++y)
        std::swap_ranges(window.pixels.begin() + y * 128, window.pixels.begin() + (y + 1) * 128, window.pixels.begin() + (127 - y) * 128);
    CHECK(same_image(expected, window));
    glDisable(GL_SCISSOR_TEST);
    multi.create(192, 96, 4); multi.bind();
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    GLint samples = 0; glGetIntegerv(GL_SAMPLES, &samples); CHECK(samples >= 4);
    CHECK(glGetError() == GL_NO_ERROR);
    glDeleteVertexArrays(1, &vao);
}

void check_plants(const char* output) {
    JobSystem jobs(2);
    MapRenderer renderer;
    renderer.init(jobs, ZL_ASSET_DIR);
    TileMap map;
    map.width = map.height = 64; map.resolution = 1; map.start = {32, 32};
    map.terrain.assign(4096, Terrain::Dirt); map.resource.assign(4096, Resource::None);
    map.level.assign(4096, 0); map.elevation.assign(4096, 0.3f);
    map.flags.assign(4096, TileFlag::Walkable | TileFlag::Buildable);
    map.moisture.assign(4096, 0.5f); map.shore_dist.assign(4096, 0); map.scenery.ready = true;
    renderer.set_map(map, jobs);
    IsometricCamera camera;
    camera.viewport = {256, 256}; camera.center = {32.5f, 32.5f}; camera.zoom = 128;
    camera.center_height = renderer.surface_height(camera.center, camera.zoom);
    RenderTarget target; target.create(256, 256, 4);
    const auto ground = [&](bool shadows) {
        MapDrawOptions options; options.shadows = shadows;
        target.bind(); renderer.draw_ground(camera, options); return target.read_pixels();
    };
    Image gallery(256 * 4, 256 * 2);
    i32 column = 0;
    for (SceneryKind kind : {SceneryKind::Tuft, SceneryKind::Bush, SceneryKind::Reed, SceneryKind::Flower}) {
        map.scenery.objects = {{kind, 0, camera.center, 1, {255, 255, 255, 255}, map.index(32, 32)}};
        renderer.refresh_objects(map);
        for (i32 rotation = 0; rotation < 4; ++rotation) {
            camera.rotation = rotation;
            const Image lit = ground(false), shaded = ground(true);
            i32 shadow_pixels = 0;
            for (size_t p = 0; p < lit.pixels.size(); ++p)
                if (int(lit.pixels[p].r) - int(shaded.pixels[p].r) > 5) ++shadow_pixels;
            std::printf("%s rotation %d: %d ground shadow pixels\n", scenery_model(kind).name, rotation, shadow_pixels);
            CHECK(shadow_pixels > 10);
            CHECK(renderer.shadow_view()->resolution == 4096);
            // Compare only the ground above; these pixels cannot be a plant's
            // own shaded faces masquerading as a cast shadow.
            if (rotation == 0) {
                target.bind(); renderer.draw(camera, {});
                const Image plant = target.read_pixels();
                for (i32 y = 0; y < 256; ++y) for (i32 x = 0; x < 256; ++x) {
                    gallery.at(column * 256 + x, y) = plant.at(x, y);
                    gallery.at(column * 256 + x, y + 256) = shaded.at(x, y);
                }
            }
            CHECK(glGetError() == GL_NO_ERROR);
        }
        map.scenery.objects[0].active = false; renderer.refresh_objects(map);
        CHECK(same_image(ground(false), ground(true)));
        ++column;
    }
    // Small real grass must remain present at ordinary play zoom while the
    // camera crosses pixel boundaries. Exclude ground texture from the metric.
    map.scenery.objects = {{SceneryKind::Tuft, 0, {32.5f,32.5f}, 1, {255,255,255,255}, map.index(32,32)}};
    renderer.refresh_objects(map); camera.rotation = 0; camera.zoom = 32;
    double minimum = 1e30, maximum = 0;
    for (i32 step = 0; step < 16; ++step) {
        camera.center.x = 32.5f + static_cast<f32>(step) / (16 * camera.zoom);
        const Image bare = ground(false);
        renderer.draw_scenery(camera, -1e30f, 1e30f);
        const Image planted = target.read_pixels();
        double energy = 0;
        for (size_t p = 0; p < bare.pixels.size(); ++p) {
            const Color a = bare.pixels[p], b = planted.pixels[p];
            energy += std::abs(int(a.r)-int(b.r))+std::abs(int(a.g)-int(b.g))+std::abs(int(a.b)-int(b.b));
        }
        minimum = std::min(minimum, energy); maximum = std::max(maximum, energy);
        CHECK(renderer.sprites_drawn() == 1);
        CHECK(!glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE));
    }
    std::printf("grass camera-motion coverage: %.0f to %.0f\n", minimum, maximum);
    CHECK(minimum > maximum * 0.75);
    CHECK(minimum > 100);
    CHECK(glGetError() == GL_NO_ERROR);
    if (output) {
        std::filesystem::create_directories(output);
        CHECK(write_png((std::filesystem::path(output)/"plant-shadows.png").string().c_str(), gallery));
    }
}
} // namespace

int main(int argc, char** argv) {
    WindowConfig config; config.visible = false; config.width = config.height = 256;
    Window window(config);
    check_multisampling();
    check_plants(argc > 1 ? argv[1] : nullptr);
    std::printf("foliage and AA GPU checks: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
