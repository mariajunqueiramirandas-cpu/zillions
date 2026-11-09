// Zillions: map viewer. For now the game is the engine plus the map generator;
// this executable lets you fly over generated maps and capture screenshots.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/clock.h"
#include "core/jobs.h"
#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "platform/window.h"
#include "render/map_renderer.h"
#include "world/mapgen.h"

using namespace zl;

namespace {

struct Options {
    MapGenParams map;
    i32 width = 1600;
    i32 height = 900;
    bool vsync = true;
    const char* screenshot = nullptr;  // render one frame to this PNG and exit
    bool bench = false;                // time the renderer off-screen and exit
    f32 zoom = 0.0f;                   // pixels per tile; 0 = default for the view
    bool whole_map = false;
    bool have_center = false;
    Vec2 center{0, 0};
    f32 time = 0.0f;
};

void usage() {
    std::puts(
        "usage: zillions [options]\n"
        "  --seed N             map seed (default 1)\n"
        "  --size N             map size in tiles (default 256)\n"
        "  --width W --height H window or screenshot size in pixels\n"
        "  --no-vsync           uncap the frame rate\n"
        "  --screenshot FILE    render one frame to a PNG and exit\n"
        "  --bench              time the renderer off-screen and exit\n"
        "  --whole-map          frame the entire map (default: the start area)\n"
        "  --center X Y         centre the view on a tile\n"
        "  --zoom PIXELS        pixels per tile\n"
        "controls: WASD/arrows or right-drag to pan, wheel to zoom, R new map,\n"
        "          G grid, F whole map, H start area, F2 screenshot, Esc quit");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const auto is = [&](const char* name) { return std::strcmp(argv[i], name) == 0; };
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (is("--seed")) o.map.seed = std::strtoull(next(), nullptr, 10);
        else if (is("--size")) o.map.size = std::atoi(next());
        else if (is("--width")) o.width = std::atoi(next());
        else if (is("--height")) o.height = std::atoi(next());
        else if (is("--no-vsync")) o.vsync = false;
        else if (is("--screenshot")) o.screenshot = next();
        else if (is("--bench")) o.bench = true;
        else if (is("--whole-map")) o.whole_map = true;
        else if (is("--zoom")) o.zoom = static_cast<f32>(std::atof(next()));
        else if (is("--time")) o.time = static_cast<f32>(std::atof(next()));
        else if (is("--center")) {
            o.center.x = static_cast<f32>(std::atof(next()));
            o.center.y = static_cast<f32>(std::atof(next()));
            o.have_center = true;
        } else {
            usage();
            return false;
        }
    }
    return o.map.size >= 64 && o.map.size <= 4096 && o.width > 0 && o.height > 0;
}

f32 fit_zoom(const TileMap& map, Vec2 viewport) {
    return std::min(viewport.x / static_cast<f32>(map.width), viewport.y / static_cast<f32>(map.height));
}

void frame_whole_map(Camera2D& camera, const TileMap& map) {
    camera.center = {static_cast<f32>(map.width) * 0.5f, static_cast<f32>(map.height) * 0.5f};
    camera.zoom = fit_zoom(map, camera.viewport);
}

void frame_start(Camera2D& camera, const TileMap& map) {
    camera.center = {static_cast<f32>(map.start.x) + 0.5f, static_cast<f32>(map.start.y) + 0.5f};
    camera.zoom = static_cast<f32>(kArtTilePixels);
}

void log_map(const TileMap& map) {
    const MapStats& s = map.stats;
    ZL_LOG("map seed %llu, %dx%d: generated in %.1f ms; %d stone / %d iron / %d gold deposits, %d fords, %.1f%% of open ground reachable",
           static_cast<unsigned long long>(map.seed), map.width, map.height, s.gen_ms, s.deposits[1], s.deposits[2], s.deposits[3], s.fords,
           s.walkable_tiles ? 100.0 * s.reachable_tiles / s.walkable_tiles : 0.0);
}

bool save_screenshot(MapRenderer& renderer, const Camera2D& camera, const MapDrawOptions& draw, const char* path) {
    RenderTarget target;
    target.create(static_cast<i32>(camera.viewport.x), static_cast<i32>(camera.viewport.y));
    target.bind();
    renderer.draw(camera, draw);
    const Image image = target.read_pixels();
    const bool ok = write_png(path, image);
    ZL_LOG("%s %s (%dx%d)", ok ? "wrote" : "could not write", path, image.width, image.height);
    return ok;
}

// Renders a fixed view repeatedly into an off-screen target. "submit" is the
// CPU time the engine spends per frame (culling, filling the instance buffer,
// issuing draw calls); "frame" also waits for the GPU to finish.
void bench_view(const char* name, MapRenderer& renderer, const Camera2D& camera) {
    RenderTarget target;
    target.create(static_cast<i32>(camera.viewport.x), static_cast<i32>(camera.viewport.y));
    target.bind();
    MapDrawOptions draw;
    constexpr int kFrames = 120;
    f64 submit_ms = 0.0;
    for (int i = 0; i < 5; ++i) renderer.draw(camera, draw);  // warm up
    glFinish();
    const Stopwatch total;
    for (int i = 0; i < kFrames; ++i) {
        draw.time = static_cast<f32>(i) / 60.0f;
        const Stopwatch submit;
        renderer.draw(camera, draw);
        submit_ms += submit.millis();
        glFinish();
    }
    std::printf("  %-10s %6u sprites   submit %7.3f ms   frame %7.3f ms\n", name, renderer.sprites_drawn(), submit_ms / kFrames, total.millis() / kFrames);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) return 2;

    JobSystem jobs;
    WindowConfig config;
    config.width = options.width;
    config.height = options.height;
    config.visible = options.screenshot == nullptr && !options.bench;
    config.vsync = options.vsync;
    Window window(config);

    MapRenderer renderer;
    {
        const Stopwatch timer;
        renderer.init(jobs);
        ZL_LOG("generated art in %.1f ms", timer.millis());
    }

    TileMap map = generate_map(options.map, jobs);
    {
        const Stopwatch timer;
        renderer.set_map(map, jobs);
        ZL_LOG("built relief and scenery in %.1f ms", timer.millis());
    }
    log_map(map);

    Camera2D camera;
    camera.viewport = {static_cast<f32>(options.width), static_cast<f32>(options.height)};
    if (options.whole_map) frame_whole_map(camera, map);
    else frame_start(camera, map);
    if (options.have_center) camera.center = options.center;
    if (options.zoom > 0.0f) camera.zoom = options.zoom;

    MapDrawOptions draw;
    draw.time = options.time;

    if (options.bench) {
        std::printf("renderer, %dx%d, 2 draw calls per frame\n", options.width, options.height);
        Camera2D view = camera;
        frame_start(view, map);
        bench_view("start", renderer, view);
        view.zoom = 12.0f;
        bench_view("zoomed out", renderer, view);
        frame_whole_map(view, map);
        bench_view("whole map", renderer, view);
        return 0;
    }

    if (options.screenshot) return save_screenshot(renderer, camera, draw, options.screenshot) ? 0 : 1;

    // --- interactive loop ---
    FixedStep sim(60.0);
    f64 last = now_seconds(), title_timer = 0.0, frame_ms_sum = 0.0;
    int frames = 0, shot_index = 0;

    while (!window.should_close()) {
        window.poll();
        const Input& in = window.input();
        const f64 now = now_seconds();
        const f64 dt = now - last;
        last = now;

        const IVec2 fb = window.framebuffer_size();
        if (fb.x <= 0 || fb.y <= 0) continue;  // minimised
        camera.viewport = {static_cast<f32>(fb.x), static_cast<f32>(fb.y)};

        if (in.pressed(Key::Escape)) window.request_close();
        if (in.pressed(Key::R) || in.pressed(Key::Space)) {
            options.map.seed += 1;
            map = generate_map(options.map, jobs);
            renderer.set_map(map, jobs);
            log_map(map);
        }
        if (in.pressed(Key::G)) draw.grid = !draw.grid;
        if (in.pressed(Key::F)) frame_whole_map(camera, map);
        if (in.pressed(Key::H)) frame_start(camera, map);

        // Camera: keyboard pan at a constant on-screen speed, drag to grab the
        // map, wheel zooms about the cursor.
        Vec2 pan{0, 0};
        if (in.down(Key::A) || in.down(Key::Left)) pan.x -= 1.0f;
        if (in.down(Key::D) || in.down(Key::Right)) pan.x += 1.0f;
        if (in.down(Key::W) || in.down(Key::Up)) pan.y -= 1.0f;
        if (in.down(Key::S) || in.down(Key::Down)) pan.y += 1.0f;
        const f32 speed = (in.down(Key::LeftShift) ? 2400.0f : 1000.0f) / camera.zoom;
        camera.center += pan * (speed * static_cast<f32>(dt));
        if (in.down(MouseButton::Right) || in.down(MouseButton::Middle)) camera.center -= in.mouse_delta / camera.zoom;
        if (in.scroll != 0.0f) camera.zoom_at(in.mouse, std::pow(1.15f, in.scroll), fit_zoom(map, camera.viewport) * 0.7f, 128.0f);
        camera.center.x = clamp(camera.center.x, 0.0f, static_cast<f32>(map.width));
        camera.center.y = clamp(camera.center.y, 0.0f, static_cast<f32>(map.height));

        // The simulation will tick here at a fixed rate; nothing moves yet.
        const int steps = sim.advance(dt);
        (void)steps;
        draw.time = static_cast<f32>(now);

        const Stopwatch frame_timer;
        RenderTarget::bind_default(fb.x, fb.y);
        renderer.draw(camera, draw);
        frame_ms_sum += frame_timer.millis();
        ++frames;

        if (in.pressed(Key::F2)) {
            const std::string path = "zillions_" + std::to_string(map.seed) + "_" + std::to_string(shot_index++) + ".png";
            save_screenshot(renderer, camera, draw, path.c_str());
        }

        window.swap_buffers();

        title_timer += dt;
        if (title_timer >= 0.5) {
            char title[200];
            std::snprintf(title, sizeof(title), "Zillions | seed %llu | %dx%d | %.0f fps | %.2f ms CPU/frame | %u sprites | 2 draw calls",
                          static_cast<unsigned long long>(map.seed), map.width, map.height, frames / title_timer, frame_ms_sum / frames,
                          renderer.sprites_drawn());
            window.set_title(title);
            title_timer = 0.0;
            frame_ms_sum = 0.0;
            frames = 0;
        }
    }
    return 0;
}
