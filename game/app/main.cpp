// Zillions: map viewer. For now the game is the engine plus the map generator;
// this executable lets you fly over generated maps and capture screenshots.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/clock.h"
#include "core/jobs.h"
#include "core/rng.h"
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
    const char* assets = nullptr;      // directory holding the painted art; null = look for it
    const char* screenshot = nullptr;  // render one frame to this PNG and exit
    bool bench = false;                // time the renderer off-screen and exit
    f32 zoom = 0.0f;                   // pixels per tile; 0 = default for the view
    bool whole_map = false;
    bool have_center = false;
    Vec2 center{0, 0};
    i32 rotation = 0;                  // clockwise quarter turns
    f32 time = 0.0f;
    MapOverlay overlay = MapOverlay::None;
    bool grid = false;
    bool shadows = true;
};

void usage() {
    std::puts(
        "usage: zillions [options]\n"
        "  --seed N             map seed (default 1)\n"
        "  --size N             map size in tiles (default 512)\n"
        "  --resolution N       tiles per world unit: 1, 2 or 4 (default 2)\n"
        "  --width W --height H window or screenshot size in pixels\n"
        "  --no-vsync           uncap the frame rate\n"
        "  --assets DIR         painted art directory (default: ./assets, then source tree)\n"
        "  --screenshot FILE    render one frame to a PNG and exit\n"
        "  --bench              time the renderer off-screen and exit\n"
        "  --whole-map          frame the entire map (default: the start area)\n"
        "  --center X Y         centre the view on a tile\n"
        "  --zoom PIXELS        pixels per tile\n"
        "  --rotation DEGREES   camera angle: 0, 90, 180 or 270\n"
        "  --overlay MODE       debug view: height or pass\n"
        "  --grid               draw tile borders\n"
        "  --no-shadows         disable cast shadows for comparisons\n"
        "controls: WASD/arrows or right-drag to pan, wheel to zoom, R new map,\n"
        "          Q/E rotate 90 degrees, V cycle overlay (height, passability),\n"
        "          G grid, F whole map, H start area, F2 screenshot, F5 reload art, Esc quit");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const auto is = [&](const char* name) { return std::strcmp(argv[i], name) == 0; };
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (is("--seed")) o.map.seed = std::strtoull(next(), nullptr, 10);
        else if (is("--size")) o.map.size = std::atoi(next());
        else if (is("--resolution")) o.map.resolution = std::atoi(next());
        else if (is("--width")) o.width = std::atoi(next());
        else if (is("--height")) o.height = std::atoi(next());
        else if (is("--no-vsync")) o.vsync = false;
        else if (is("--assets")) {
            if (i + 1 >= argc) {
                usage();
                return false;
            }
            o.assets = argv[++i];
        }
        else if (is("--screenshot")) o.screenshot = next();
        else if (is("--bench")) o.bench = true;
        else if (is("--whole-map")) o.whole_map = true;
        else if (is("--zoom")) o.zoom = static_cast<f32>(std::atof(next()));
        else if (is("--rotation")) {
            const i32 degrees = std::atoi(next());
            if (degrees % 90 != 0) { usage(); return false; }
            o.rotation = (degrees / 90 % 4 + 4) % 4;
        }
        else if (is("--time")) o.time = static_cast<f32>(std::atof(next()));
        else if (is("--grid")) o.grid = true;
        else if (is("--no-shadows")) o.shadows = false;
        else if (is("--overlay")) {
            const char* mode = next();
            if (!std::strcmp(mode, "height")) o.overlay = MapOverlay::Height;
            else if (!std::strcmp(mode, "pass")) o.overlay = MapOverlay::Passability;
            else {
                usage();
                return false;
            }
        }
        else if (is("--center")) {
            o.center.x = static_cast<f32>(std::atof(next()));
            o.center.y = static_cast<f32>(std::atof(next()));
            o.have_center = true;
        } else {
            usage();
            return false;
        }
    }
    const i32 res = o.map.resolution;
    if ((res != 1 && res != 2 && res != 4) || o.map.size < 64 * res || o.map.size > 4096 || o.width <= 0 || o.height <= 0) {
        usage();
        return false;
    }
    return true;
}

// The directory holding the painted art: the one asked for, else ./assets,
// else the copy in the source tree this binary was built from. Empty if there
// is none, in which case the renderer draws placeholder art in code.
std::string find_assets(const char* requested) {
    if (requested) return requested;
    std::error_code error;
    if (std::filesystem::is_directory("assets", error)) return "assets";
#ifdef ZL_ASSET_DIR
    if (std::filesystem::is_directory(ZL_ASSET_DIR, error)) return ZL_ASSET_DIR;
#endif
    return {};
}

f32 fit_zoom(const TileMap& map, Vec2 viewport) {
    const f32 diagonal = static_cast<f32>(map.width + map.height);
    return std::min(viewport.x / (diagonal * IsometricCamera::kX), viewport.y / (diagonal * IsometricCamera::kY));
}

void frame_start(IsometricCamera& camera, const TileMap& map, const MapRenderer& renderer) {
    camera.center = {static_cast<f32>(map.start.x) + 0.5f, static_cast<f32>(map.start.y) + 0.5f};
    camera.center_height = renderer.ground_height(camera.center);
    camera.zoom = static_cast<f32>(kArtTilePixels);
}

void log_map(const TileMap& map) {
    const MapStats& s = map.stats;
    ZL_LOG("map seed %llu, %dx%d: generated in %.1f ms; %d stone / %d iron / %d gold deposits, %d fords, %.1f%% of open ground reachable",
           static_cast<unsigned long long>(map.seed), map.width, map.height, s.gen_ms, s.deposits[1], s.deposits[2], s.deposits[3], s.fords,
           s.walkable_tiles ? 100.0 * s.reachable_tiles / s.walkable_tiles : 0.0);
}

bool save_screenshot(MapRenderer& renderer, const IsometricCamera& camera, const MapDrawOptions& draw, const char* path) {
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
void bench_view(const char* name, MapRenderer& renderer, const IsometricCamera& camera) {
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

// Stress test for the unit path that does not exist yet: `count` sprites that
// all move every frame, written straight into GPU memory by every core. "fill"
// is the engine's own cost; "frame" adds the draw and waits for the GPU.
void bench_horde(u32 count, const MapRenderer& renderer, const TileMap& map, JobSystem& jobs, SpriteBatch& batch, const Camera2D& camera) {
    RenderTarget target;
    target.create(static_cast<i32>(camera.viewport.x), static_cast<i32>(camera.viewport.y));
    target.bind();
    glClearColor(0.1f, 0.12f, 0.1f, 1.0f);

    const SpriteSet& set = renderer.art().bush;
    const SpriteDef* defs = renderer.art().defs.data() + set.first;
    const f32 size = static_cast<f32>(map.width);
    const auto fill = [&](SpriteInstance* out, f32 time, JobSystem& pool) {
        pool.parallel_for(static_cast<i32>(count), 4096, [&](i32 begin, i32 end) {
            for (i32 i = begin; i < end; ++i) {
                // A stand-in for simulation output: every sprite drifts on its own heading.
                const u32 h = hash32(static_cast<u32>(i) * 2654435761u + 1u);
                const f32 x0 = hash_to_unit(h) * size, y0 = hash_to_unit(hash32(h + 1)) * size;
                const f32 vx = hash_to_unit(hash32(h + 2)) - 0.5f, vy = hash_to_unit(hash32(h + 3)) - 0.5f;
                f32 x = x0 + vx * time * 3.0f, y = y0 + vy * time * 3.0f;
                x -= size * std::floor(x / size);
                y -= size * std::floor(y / size);
                const SpriteDef& d = defs[h % static_cast<u32>(set.count)];
                out[i] = {x - d.anchor_x * 0.6f, y - d.anchor_y * 0.6f, d.width * 0.6f, d.height * 0.6f, d.u0, d.v0, d.u1, d.v1, 0xFFFFFFFFu};
            }
        });
    };

    JobSystem single(1);
    constexpr int kFrames = 40;
    f64 fill_ms = 0.0, fill_single_ms = 0.0;
    glFinish();
    const Stopwatch total;
    for (int frame = 0; frame < kFrames; ++frame) {
        glClear(GL_COLOR_BUFFER_BIT);
        SpriteInstance* out = batch.begin(count);
        const Stopwatch timer;
        fill(out, static_cast<f32>(frame) / 60.0f, jobs);
        fill_ms += timer.millis();
        batch.draw(count, camera, renderer.sprite_atlas());
        glFinish();
    }
    const f64 frame_ms = total.millis() / kFrames;
    // The same fill on one thread, for the scaling figure.
    for (int frame = 0; frame < 8; ++frame) {
        SpriteInstance* out = batch.begin(count);
        const Stopwatch timer;
        fill(out, static_cast<f32>(frame) / 60.0f, single);
        fill_single_ms += timer.millis();
        batch.draw(0, camera, renderer.sprite_atlas());
    }
    std::printf("  %7u moving sprites   fill %6.3f ms on %d threads (%6.3f ms on 1)   frame %8.3f ms\n", count, fill_ms / kFrames,
                jobs.thread_count(), fill_single_ms / 8, frame_ms);
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
        const std::string assets = find_assets(options.assets);
        if (assets.empty()) ZL_LOG("art: no assets directory found; drawing placeholder art in code");
        const Stopwatch timer;
        renderer.init(jobs, assets.empty() ? nullptr : assets.c_str());
        const TerrainArt& art = renderer.art();
        ZL_LOG("art: %s ground, %s sprites, ready in %.1f ms", art.painted_ground ? "painted" : "placeholder",
               art.painted_sprites ? "painted" : "placeholder", timer.millis());
    }

    TileMap map = generate_map(options.map, jobs);
    {
        const Stopwatch timer;
        renderer.set_map(map, jobs);
        ZL_LOG("built relief and scenery in %.1f ms", timer.millis());
    }
    log_map(map);

    IsometricCamera camera;
    camera.rotation = options.rotation;
    camera.viewport = {static_cast<f32>(options.width), static_cast<f32>(options.height)};
    if (options.whole_map) renderer.frame_map(camera);
    else frame_start(camera, map, renderer);
    if (options.have_center) {
        camera.center = options.center;
        camera.center_height = renderer.ground_height(camera.center);
    }
    if (options.zoom > 0.0f) camera.zoom = options.zoom;

    MapDrawOptions draw;
    draw.time = options.time;
    draw.overlay = options.overlay;
    draw.grid = options.grid;
    draw.shadows = options.shadows;

    if (options.bench) {
        std::printf("isometric renderer, %dx%d, up to 3 visible draws plus 2 cached shadow draws\n", options.width, options.height);
        IsometricCamera view = camera;
        frame_start(view, map, renderer);
        bench_view("start", renderer, view);
        view.zoom = 12.0f;
        bench_view("zoomed out", renderer, view);
        renderer.frame_map(view);
        bench_view("whole map", renderer, view);

        std::printf("sprite batch, 1 draw call per frame\n");
        SpriteBatch horde;
        horde.init();
        Camera2D stress;
        stress.viewport = camera.viewport;
        stress.center = {static_cast<f32>(map.width) * 0.5f, static_cast<f32>(map.height) * 0.5f};
        stress.zoom = std::min(stress.viewport.x / static_cast<f32>(map.width), stress.viewport.y / static_cast<f32>(map.height));
        for (const u32 count : {10000u, 100000u, 250000u}) bench_horde(count, renderer, map, jobs, horde, stress);
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
            camera.center_height = renderer.ground_height(camera.center);
        }
        if (in.pressed(Key::G)) draw.grid = !draw.grid;
        if (in.pressed(Key::V)) draw.overlay = static_cast<MapOverlay>((static_cast<int>(draw.overlay) + 1) % static_cast<int>(MapOverlay::Count));
        if (in.pressed(Key::Q) || in.pressed(Key::E)) {
            Vec2 focus;
            if (renderer.pick(camera, camera.viewport * 0.5f, focus)) {
                camera.center = focus;
                camera.center_height = renderer.surface_height(focus, camera.zoom);
            }
            camera.turn(in.pressed(Key::E) ? 1 : -1);
        }
        if (in.pressed(Key::F)) renderer.frame_map(camera);
        if (in.pressed(Key::H)) frame_start(camera, map, renderer);
        if (in.pressed(Key::F5)) {
            const std::string assets = find_assets(options.assets);
            const Stopwatch timer;
            renderer.reload_art(jobs, assets.empty() ? nullptr : assets.c_str());
            const TerrainArt& art = renderer.art();
            ZL_LOG("art reloaded: %s ground, %s sprites, ready in %.1f ms", art.painted_ground ? "painted" : "placeholder",
                   art.painted_sprites ? "painted" : "placeholder", timer.millis());
        }

        // Camera: keyboard pan at a constant on-screen speed, drag to grab the
        // map, wheel zooms about the cursor.
        Vec2 pan{0, 0};
        if (in.down(Key::A) || in.down(Key::Left)) pan.x -= 1.0f;
        if (in.down(Key::D) || in.down(Key::Right)) pan.x += 1.0f;
        if (in.down(Key::W) || in.down(Key::Up)) pan.y -= 1.0f;
        if (in.down(Key::S) || in.down(Key::Down)) pan.y += 1.0f;
        const f32 speed = in.down(Key::LeftShift) ? 2400.0f : 1000.0f;
        if (length(pan) > 0.0f) pan = pan / length(pan);
        camera.center += camera.screen_delta_to_world(pan * (speed * static_cast<f32>(dt)));
        if (in.down(MouseButton::Right) || in.down(MouseButton::Middle)) camera.center -= camera.screen_delta_to_world(in.mouse_delta);
        if (in.scroll != 0.0f) {
            Vec2 anchor;
            const bool hit = renderer.pick(camera, in.mouse, anchor);
            const f32 height = hit ? renderer.surface_height(anchor, camera.zoom) : camera.center_height;
            if (!hit) anchor = camera.screen_to_world(in.mouse, height);
            camera.zoom = clamp(camera.zoom * std::pow(1.15f, in.scroll), fit_zoom(map, camera.viewport) * 0.5f, 128.0f);
            const f32 new_height = hit ? renderer.surface_height(anchor, camera.zoom) : height;
            camera.center += anchor - camera.screen_to_world(in.mouse, new_height);
        }
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
            std::snprintf(title, sizeof(title), "Zillions | seed %llu | %dx%d | %.0f fps | %.2f ms CPU/frame | %u sprites | isometric %d deg | Q/E rotate",
                          static_cast<unsigned long long>(map.seed), map.width, map.height, frames / title_timer, frame_ms_sum / frames,
                          renderer.sprites_drawn(), camera.rotation * 90);
            window.set_title(title);
            title_timer = 0.0;
            frame_ms_sum = 0.0;
            frames = 0;
        }
    }
    return 0;
}
