// Zillions. For now the game is the engine, the map generator and one
// character: this executable lets you walk the player's character about a
// generated map, fly the camera over it and capture screenshots.
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <string>
#include <vector>

#include "actor/human.h"
#include "actor/player.h"
#include "actor/plague.h"
#include "core/clock.h"
#include "core/jobs.h"
#include "core/rng.h"
#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "platform/window.h"
#include "render/figure_renderer.h"
#include "render/map_renderer.h"
#include "render/model_renderer.h"
#include "render/survival_ui.h"
#include "survival/survival.h"
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
    const char* record_demo = nullptr; // write deterministic demo PNGs + a state manifest
    f64 demo_seconds = 8.0;
    i32 demo_fps = 20;
    f32 zoom = 0.0f;                   // pixels per tile; 0 = default for the view
    bool whole_map = false;
    bool have_center = false;
    Vec2 center{0, 0};
    f32 time = 0.0f;
    MapOverlay overlay = MapOverlay::None;
    bool grid = false;
    bool shadows = true;
    bool collisions = false;
    i32 rotation = 0;

    bool character = true;                  // put the player's character on the map
    FigureStyle character_style = FigureStyle::Painted;
    const char* character_sheet = nullptr;  // render the character's poses to this PNG and exit
    bool have_player = false;
    Vec2 player{0, 0};                      // where the character stands, in tiles
    HumanMotion pose;                       // its pose in a screenshot
    u64 look_seed = 0;                      // 0: the hero; otherwise a random person
    bool hood_up = false;
    HumanTool tool = HumanTool::None;
    f32 work_phase = 0;
};

// Pixels per tile the camera starts at when it follows the character, the
// tactical city-builder overview in management mode, and the closest it will go.
constexpr f32 kFollowZoom = 32.0f;
constexpr f32 kManagementZoom = 12.0f;
constexpr f32 kMaxZoom = 256.0f;
constexpr f32 kEdgePanMargin = 36.0f;

void usage() {
    std::puts(
        "usage: zillions [options]\n"
        "  --seed N             map seed (default 1)\n"
        "  --size N             map size in tiles (default 4096; 5.12 km at player scale)\n"
        "  --resolution N       tiles per world unit: 1, 2 or 4 (default 2)\n"
        "  --width W --height H window or screenshot size in pixels\n"
        "  --no-vsync           uncap the frame rate\n"
        "  --assets DIR         painted art directory (default: ./assets, then source tree)\n"
        "  --screenshot FILE    render one frame to a PNG and exit\n"
        "  --bench              time the renderer off-screen and exit\n"
        "  --record-demo DIR    record the player demo as PNGs + frames.json and exit\n"
        "  --demo-seconds N     demo length, 0.1..30 seconds (default 8)\n"
        "  --demo-fps N         demo frame rate, 1..60 (default 20)\n"
        "  --whole-map          frame the entire map (default: the start area)\n"
        "  --center X Y         centre the view on a tile\n"
        "  --zoom PIXELS        pixels per tile\n"
        "  --rotation DEGREES   camera angle: 0, 90, 180 or 270\n"
        "  --no-shadows         disable cast shadows for comparisons\n"
        "  --colliders          show object collision footprints and heights\n"
        "  --time SECONDS       screenshot animation time\n"
        "  --overlay MODE       debug view: height or pass\n"
        "  --grid               draw tile borders\n"
        "  --no-character       the map alone, as a free-flying viewer\n"
        "  --character-style S  painted (default) or smooth\n"
        "  --character-sheet F  render the character's poses to a PNG and exit\n"
        "  --player X Y         where the character stands (default: the start)\n"
        "  --heading DEG        screenshots: the way it faces (0 east, 90 towards the viewer)\n"
        "  --speed M --phase P  screenshots: its speed in m/s and place in the stride, 0..1\n"
        "  --look N             a different person for every N (default: the hero)\n"
        "  --hood               hood up\n"
        "  --tool axe|pickaxe   pose a held tool for screenshots\n"
        "  --work-phase P       screenshot tool swing phase, 0..1\n"
        "controls: WASD/arrows walk, Shift sprint, Ctrl stroll, wheel zoom,\n"
        "          hold left mouse near resource bases to gather, I inventory, B crafting,\n"
        "          T rotate a chest during placement,\n"
        "          Space management mode (border/WASD/right-drag pan, autonomous character),\n"
        "          Q/E rotate 90 degrees, C new look, X hood, 1 the hero's look,\n"
        "          Ctrl+R new map (R in map-only mode), V overlay, G grid,\n"
        "          F whole map, H centre on character, F2 screenshot, F3 colliders, F5 reload art,\n"
        "          F6 painted/smooth character, Esc close panel/cancel placement/quit");
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
        else if (is("--record-demo")) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || std::strncmp(argv[i + 1], "--", 2) == 0) { usage(); return false; }
            o.record_demo = argv[++i];
        }
        else if (is("--demo-seconds")) o.demo_seconds = std::atof(next());
        else if (is("--demo-fps")) o.demo_fps = std::atoi(next());
        else if (is("--whole-map")) o.whole_map = true;
        else if (is("--zoom")) o.zoom = static_cast<f32>(std::atof(next()));
        else if (is("--no-shadows")) o.shadows = false;
        else if (is("--colliders")) o.collisions = true;
        else if (is("--rotation")) {
            const i32 degrees = std::atoi(next());
            if (degrees % 90 != 0) { usage(); return false; }
            o.rotation = (degrees / 90 % 4 + 4) % 4;
        }
        else if (is("--time")) o.time = static_cast<f32>(std::atof(next()));
        else if (is("--grid")) o.grid = true;
        else if (is("--overlay")) {
            const char* mode = next();
            if (!std::strcmp(mode, "height")) o.overlay = MapOverlay::Height;
            else if (!std::strcmp(mode, "pass")) o.overlay = MapOverlay::Passability;
            else {
                usage();
                return false;
            }
        }
        else if (is("--no-character")) o.character = false;
        else if (is("--character-style")) {
            const char* style = next();
            if (!std::strcmp(style, "painted")) o.character_style = FigureStyle::Painted;
            else if (!std::strcmp(style, "smooth")) o.character_style = FigureStyle::Smooth;
            else { usage(); return false; }
        }
        else if (is("--character-sheet")) o.character_sheet = next();
        else if (is("--player")) {
            o.player.x = static_cast<f32>(std::atof(next()));
            o.player.y = static_cast<f32>(std::atof(next()));
            o.have_player = true;
        }
        else if (is("--heading")) o.pose.heading = static_cast<f32>(std::atof(next())) * kPi / 180.0f;
        else if (is("--speed")) o.pose.speed = static_cast<f32>(std::atof(next()));
        else if (is("--phase")) o.pose.phase = static_cast<f32>(std::atof(next()));
        else if (is("--look")) o.look_seed = std::strtoull(next(), nullptr, 10);
        else if (is("--hood")) o.hood_up = true;
        else if (is("--tool")) {
            const char* tool = next();
            if (!std::strcmp(tool,"axe")) o.tool = HumanTool::Axe;
            else if (!std::strcmp(tool,"pickaxe")) o.tool = HumanTool::Pickaxe;
            else { usage(); return false; }
        }
        else if (is("--work-phase")) o.work_phase = static_cast<float>(std::atof(next()));
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
    if (o.record_demo && (!o.character || o.screenshot || o.character_sheet || o.bench ||
        !std::isfinite(o.demo_seconds) || o.demo_seconds < 0.1 || o.demo_seconds > 30.0 ||
        o.demo_fps < 1 || o.demo_fps > 60 || o.width > 2048 || o.height > 2048)) {
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

void frame_whole_map(IsometricCamera& camera, const TileMap&, const MapRenderer& renderer) { renderer.frame_map(camera); }

void frame_start(IsometricCamera& camera, const TileMap& map, const MapRenderer& renderer) {
    camera.center = {static_cast<f32>(map.start.x) + 0.5f, static_cast<f32>(map.start.y) + 0.5f};
    camera.zoom = static_cast<f32>(kArtTilePixels);
    camera.center_height = renderer.surface_height(camera.center, camera.zoom);
}

void log_map(const TileMap& map) {
    const MapStats& s = map.stats;
    ZL_LOG("map seed %llu, %dx%d: generated in %.1f ms; %d stone / %d iron / %d gold deposits, %d fords, %.1f%% of open ground reachable",
           static_cast<unsigned long long>(map.seed), map.width, map.height, s.gen_ms, s.deposits[1], s.deposits[2], s.deposits[3], s.fords,
           s.walkable_tiles ? 100.0 * s.reachable_tiles / s.walkable_tiles : 0.0);
}

HumanLook make_look(u64 seed, bool hood_up) {
    HumanLook look = seed == 0 ? hero_look() : random_look(seed);
    if (hood_up) look.hood_up = true;
    return look;
}

// Interleave the character with scenery at its rotated ground depth. Terrain
// depth handles cliffs; foreground scenery fades around the projected body.
// The plague doctor is a real skinned mesh and writes depth; the painted
// procedural figure is the missing-model fallback.
struct Scene {
    MapRenderer& map;
    FigureRenderer& figures;
    ModelRenderer* models = nullptr;
    PlagueAnimator* animator = nullptr;
    bool plague = false;

    void draw(const IsometricCamera& camera, const MapDrawOptions& options, const Figure* figure, Vec2 position,
              f32 extra_marker = 0.0f, const PlagueDraw* plague_draw = nullptr) {
        if (plague && models && animator && plague_draw && animator->current().valid) {
            const Vec2 rotated = camera.rotate(plague_draw->position);
            const f32 depth = rotated.x + rotated.y;
            Vec2 lo{0, 0}, hi{0, 0};
            const bool reveal_ok = models->reveal_bounds(camera, *plague_draw, lo, hi);
            const SeeThrough reveal{lo, hi};
            map.draw_ground(camera, options);
            map.draw_scenery(camera, -1.0e30f, depth);
            models->draw(camera, map.shadow_view(), map.depth_scale(), *plague_draw, animator->current());
            map.draw_scenery(camera, depth, 1.0e30f, reveal_ok ? &reveal : nullptr);
            models->draw_marker(camera, *plague_draw);
            if (options.collisions) map.draw_collisions(camera);
            return;
        }
        if (!figure || figure->parts.empty()) { map.draw(camera, options); return; }
        const f32 height = map.surface_height(position, camera.zoom);
        const Vec2 standing = camera.project(position, height);
        Vec2 lo{1.0e9f, 1.0e9f}, hi{-1.0e9f, -1.0e9f};
        for (const FigurePart& part : figure->parts) {
            for (const Vec3 p : {part.a, part.b}) {
                const Vec2 q = standing + camera.project({p.x, p.y}, p.z - figure->waterline);
                const f32 radius = std::max(part.ra, part.rb);
                lo.x = std::min(lo.x, q.x - radius); lo.y = std::min(lo.y, q.y - radius);
                hi.x = std::max(hi.x, q.x + radius); hi.y = std::max(hi.y, q.y + radius);
            }
        }
        const SeeThrough reveal{lo, hi};
        const f32 marker = saturate(std::max(1.0f - smoothstep(9.0f, 16.0f, camera.zoom), extra_marker));
        const FigureDraw item{figure, position, options.time, marker, height};
        const Vec2 rotated = camera.rotate(position);
        const f32 depth = rotated.x + rotated.y;
        map.draw_ground(camera, options);
        map.draw_scenery(camera, -1.0e30f, depth);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        figures.draw(&item, 1, camera.plane_camera(), &camera, map.shadow_view(), map.depth_scale());
        glDepthMask(GL_TRUE);
        map.draw_scenery(camera, depth, 1.0e30f, &reveal);
        if (options.collisions) map.draw_collisions(camera);
    }
};

bool save_screenshot(Scene& scene, const IsometricCamera& camera, const MapDrawOptions& draw, const Figure* figure, Vec2 position, const char* path, const PlagueDraw* plague = nullptr) {
    RenderTarget target;
    target.create(static_cast<i32>(camera.viewport.x), static_cast<i32>(camera.viewport.y), 4);
    target.bind();
    scene.draw(camera, draw, figure, position, 0.0f, plague);
    const Image image = target.read_pixels();
    const bool ok = write_png(path, image);
    ZL_LOG("%s %s (%dx%d)", ok ? "wrote" : "could not write", path, image.width, image.height);
    return ok;
}

// HumanMotion::speed quickens the procedural stride for isometric
// foreshortening. The skinned model's feet must follow the actual ground
// speed, so use the velocity projected on the current heading.
f32 model_ground_speed(const Player& player) {
    const f32 heading = player.motion().heading;
    const Vec2 forward{std::cos(heading), std::sin(heading)};
    return std::max(dot(player.velocity(), forward), 0.0f) / kTilesPerMetre;
}

struct DemoStage {
    f32 end;
    const char* label;
    PlayerInput input;
};

const DemoStage& demo_stage(f32 progress) {
    static constexpr DemoStage stages[] = {
        {0.08f, "Idle", {}},
        {0.34f, "Walk - profile", {{1, 0}, false, true}},
        {0.56f, "Walk - turn towards camera", {{0, 1}, false, true}},
        {0.76f, "Jog - turn left", {{-1, 0}, false, false}},
        {0.90f, "Sprint - turn away", {{0, -1}, true, false}},
        {1.01f, "Stop and settle", {}},
    };
    for (const DemoStage& stage : stages) if (progress < stage.end) return stage;
    return stages[5];
}

// Offline time is independent of GPU speed. Simulate at the same 60 Hz as
// live play, then interpolate at the requested recording rate. Record normal
// Player inputs so collision, acceleration, turning and gait remain real.
bool record_player_demo(Scene& scene, const TileMap& map, Player& player, IsometricCamera camera, MapDrawOptions draw, const Options& options) {
    const std::filesystem::path directory(options.record_demo);
    std::error_code error;
    if (std::filesystem::exists(directory, error) && (!std::filesystem::is_directory(directory, error) || !std::filesystem::is_empty(directory, error))) {
        ZL_LOG("recording directory must be new or empty: %s", options.record_demo);
        return false;
    }
    std::filesystem::create_directories(directory, error);
    if (error) { ZL_LOG("cannot create recording directory: %s", error.message().c_str()); return false; }
    std::ofstream manifest(directory / "frames.json");
    if (!manifest) { ZL_LOG("cannot write demo manifest"); return false; }
    const i32 frames = static_cast<i32>(std::ceil(options.demo_seconds * options.demo_fps - 1.0e-9));
    const bool follow = !options.have_center && !options.whole_map;
    if (follow) { camera.center = player.position(); camera.center_height = scene.map.surface_height(camera.center, camera.zoom) + 0.5f; }
    if (options.zoom <= 0 && !options.whole_map) camera.zoom = 128;
    manifest << std::setprecision(9) << "{\"fps\":" << options.demo_fps << ",\"seconds\":" << options.demo_seconds
             << ",\"width\":" << options.width << ",\"height\":" << options.height << ",\"seed\":" << map.seed
             << ",\"map_size\":" << map.width << ",\"resolution\":" << map.resolution << ",\"zoom\":" << camera.zoom
             << ",\"look\":" << options.look_seed << ",\"hood\":" << (player.look.hood_up ? "true" : "false") << ",\"simulation_hz\":60"
             << ",\"style\":\"" << (scene.plague ? "plague" : (scene.figures.style() == FigureStyle::Painted ? "painted" : "smooth")) << "\",\"frames\":[\n";
    Vec2 previous_camera = camera.center;
    RenderTarget target;
    target.create(options.width, options.height, 4);
    Figure figure;
    i32 tick = 0;
    constexpr f32 dt = 1.0f / 60.0f;
    for (i32 frame = 0; frame < frames; ++frame) {
        const i32 whole = frame * 60 / options.demo_fps;
        const f32 alpha = static_cast<f32>(frame * 60 % options.demo_fps) / static_cast<f32>(options.demo_fps);
        while (tick <= whole) {
            previous_camera = camera.center;
            const f32 time = static_cast<f32>(tick) / 60.0f;
            player.step(map, demo_stage(static_cast<f32>(time / options.demo_seconds)).input, dt);
            if (scene.plague && scene.animator) {
                HumanMotion model_motion = player.motion();
                model_motion.speed = model_ground_speed(player);
                scene.animator->update(model_motion, dt);
            }
            if (follow) camera.center += (player.position() - camera.center) * (1.0f - std::exp(-dt * 9.0f));
            ++tick;
        }
        IsometricCamera view = camera;
        view.center = lerp(previous_camera, camera.center, alpha);
        if (follow) view.center_height = scene.map.surface_height(player.position_at(alpha), view.zoom) + 0.5f;
        const f32 time = static_cast<f32>(frame) / static_cast<f32>(options.demo_fps);
        draw.time = options.time + time;
        const HumanMotion motion = player.motion_at(alpha);
        const Vec2 position = player.position_at(alpha);
        build_human(player.look, motion, figure);
        PlagueDraw plague_draw;
        if (scene.plague) {
            plague_draw.position = position;
            plague_draw.ground_height = scene.map.surface_height(position, view.zoom);
            plague_draw.heading = motion.heading;
            plague_draw.wade = motion.wade;
            plague_draw.marker = 0.0f;
        }
        target.bind();
        scene.draw(view, draw, &figure, position, 0.0f, scene.plague ? &plague_draw : nullptr);
        char filename[32];
        std::snprintf(filename, sizeof(filename), "frame_%05d.png", frame);
        const std::string path = (directory / filename).string();
        if (!write_png(path.c_str(), target.read_pixels())) { ZL_LOG("cannot write %s", path.c_str()); return false; }
        manifest << (frame ? ",\n" : "") << "{\"file\":\"" << filename << "\",\"time\":" << time
                 << ",\"stage\":\"" << demo_stage(static_cast<f32>(time / options.demo_seconds)).label << "\",\"x\":" << position.x
                 << ",\"y\":" << position.y << ",\"speed\":" << motion.speed << ",\"heading\":" << motion.heading
                 << ",\"phase\":" << motion.phase << ",\"body_activity\":" << motion.body_activity
                 << ",\"body_run\":" << motion.body_run << ",\"rig\":{";
        const HumanSkeleton rig = pose_human(player.look, motion);
        const f32 scale = kTilesPerMetre * player.look.height;
        const Vec2 forward{std::cos(motion.heading), std::sin(motion.heading)}, right{-forward.y, forward.x};
        const auto bones = [&](const char* name, std::initializer_list<Vec3> joints, bool first = false) {
            manifest << (first ? "" : ",") << "\"" << name << "\":[";
            bool first_joint = true;
            for (Vec3 joint : joints) {
                const Vec3 world{(right.x * joint.x + forward.x * joint.y) * scale,
                                 (right.y * joint.x + forward.y * joint.y) * scale, joint.z * scale};
                const Vec2 screen = view.world_to_screen(position + Vec2{world.x, world.y},
                    scene.map.surface_height(position, view.zoom) + world.z - figure.waterline);
                manifest << (first_joint ? "" : ",") << "[" << screen.x << "," << screen.y << "]";
                first_joint = false;
            }
            manifest << "]";
        };
        bones("spine", {rig.pelvis, rig.waist, rig.chest, rig.neck, rig.head}, true);
        for (int i = 0; i < 2; ++i) {
            bones(i == 0 ? "left_arm" : "right_arm", {rig.shoulder[i], rig.elbow[i], rig.wrist[i], rig.fingertip[i]});
            bones(i == 0 ? "left_thumb" : "right_thumb", {rig.wrist[i], rig.thumb[i]});
            bones(i == 0 ? "left_leg" : "right_leg", {rig.hip[i], rig.knee[i], rig.ankle[i], rig.heel[i], rig.ball[i], rig.toe[i]});
        }
        manifest << "}}";
        if (frame % options.demo_fps == 0) ZL_LOG("recording demo: %d/%d frames", frame + 1, frames);
    }
    manifest << "\n]}\n";
    manifest.close();
    if (!manifest) { ZL_LOG("could not finish demo manifest"); return false; }
    ZL_LOG("recorded %d frames to %s", frames, options.record_demo);
    return true;
}

// The character from every side and through its strides, on the ground of the
// start area: a row standing, turned through eight headings; walking and
// running in profile, towards the viewer and away; seeded people; and explicit
// front/back hair, beard, hood and blink variants.
bool save_character_sheet(Scene& scene, const TileMap& map, const Options& options) {
    constexpr int kColumns = 8, kRows = 8;
    constexpr f32 kCellWidth = 1.5f, kCellHeight = 1.9f;
    const f32 zoom = options.zoom > 0.0f ? options.zoom : 96.0f;

    IsometricCamera camera;
    camera.rotation = options.rotation;
    camera.zoom = zoom;
    camera.viewport = {std::round(kColumns * kCellWidth * zoom), std::round(kRows * kCellHeight * zoom)};
    camera.center = {static_cast<f32>(map.start.x) + 0.5f, static_cast<f32>(map.start.y) + 0.5f};
    const Camera2D catalogue = static_cast<const Camera2D&>(camera);
    const Vec2 corner = catalogue.visible_min();

    const HumanLook hero = make_look(options.look_seed, options.hood_up);
    std::vector<Figure> figures(static_cast<size_t>(kColumns * kRows));
    std::vector<FigureDraw> draws;
    for (int row = 0; row < kRows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            const f32 step = static_cast<f32>(column) / static_cast<f32>(kColumns);
            HumanLook look = hero;
            HumanMotion motion;
            motion.time = options.time + 0.9f + static_cast<f32>(column) * 1.37f;
            switch (row) {
                case 0: motion.heading = kPi * 0.5f - step * 2.0f * kPi; break;                       // turning on the spot
                case 1: motion.heading = 0.0f; motion.speed = kWalkSpeed; motion.phase = step; break;   // walking east
                case 2: motion.heading = kPi * 0.36f; motion.speed = kWalkSpeed; motion.phase = step; break;  // walking towards the viewer
                case 3: motion.heading = 0.0f; motion.speed = kJogSpeed; motion.phase = step; break;    // running east
                case 4: motion.heading = -kPi * 0.68f; motion.speed = kSprintSpeed; motion.phase = step; break;  // sprinting away
                case 5:
                    look = random_look(static_cast<u64>(column) + 1 + options.look_seed * 8);
                    motion.heading = kPi * (0.5f - 0.16f * static_cast<f32>(column % 3 - 1));
                    break;
                default:
                    // Guaranteed coverage of all five styles, a beard, a
                    // raised hood and a blink, from the front and the back.
                    look.hair_style = static_cast<HairStyle>(column % static_cast<int>(HairStyle::Count));
                    look.beard = column == 5;
                    look.hood_up = column == 6;
                    motion.heading = row == 6 ? kPi * 0.5f : -kPi * 0.5f;
                    motion.time = column == 7 ? (0.2f + hash_to_unit(hash32(97u)) * 0.6f) * 3.4f : 0.9f;
                    break;
            }
            Figure& figure = figures[static_cast<size_t>(row * kColumns + column)];
            build_human(look, motion, figure);
            const Vec2 feet{corner.x + (static_cast<f32>(column) + 0.5f) * kCellWidth, corner.y + (static_cast<f32>(row) + 0.80f) * kCellHeight};
            draws.push_back({&figure, feet, options.time, 0.0f});
        }
    }

    RenderTarget target;
    target.create(static_cast<i32>(camera.viewport.x), static_cast<i32>(camera.viewport.y));
    target.bind();
    MapDrawOptions draw;
    draw.time = options.time;
    scene.map.draw_ground(camera, draw);
    glDisable(GL_DEPTH_TEST);
    scene.figures.draw(draws.data(), static_cast<u32>(draws.size()), catalogue);
    const Image image = target.read_pixels();
    const bool ok = write_png(options.character_sheet, image);
    ZL_LOG("%s %s (%dx%d): %d figures, %zu parts each", ok ? "wrote" : "could not write", options.character_sheet, image.width, image.height,
           kColumns * kRows, figures[0].parts.size());
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

// Isolate the procedural figure, including pose construction and data upload.
// A close-up covers many more fragments than a character at gameplay zoom.
void bench_figure(FigureRenderer& renderer, const HumanLook& look, Camera2D camera) {
    RenderTarget target;
    target.create(static_cast<i32>(camera.viewport.x), static_cast<i32>(camera.viewport.y));
    target.bind();
    camera.center = {0.0f, -0.5f};
    constexpr int kFrames = 30;
    for (f32 zoom : {6.0f, kFollowZoom, kMaxZoom}) {
        camera.zoom = zoom;
        Figure figure;
        f64 submit_ms = 0.0;
        for (int i = 0; i < 3; ++i) {
            build_human(look, HumanMotion{}, figure);
            const FigureDraw item{&figure, {0, 0}, 0, zoom < 9.0f ? 1.0f : 0.0f};
            renderer.draw(&item, 1, camera);
        }
        glFinish();
        const Stopwatch total;
        for (int i = 0; i < kFrames; ++i) {
            glClear(GL_COLOR_BUFFER_BIT);
            const Stopwatch submit;
            HumanMotion motion;
            motion.speed = kJogSpeed;
            motion.phase = static_cast<f32>(i) / kFrames;
            motion.time = static_cast<f32>(i) / 60.0f;
            build_human(look, motion, figure);
            const FigureDraw item{&figure, {0, 0}, motion.time, zoom < 9.0f ? 1.0f : 0.0f};
            renderer.draw(&item, 1, camera);
            submit_ms += submit.millis();
            glFinish();
        }
        std::printf("  figure %3.0f px/tile  %zu parts  %u calls   submit %7.3f ms   frame %7.3f ms\n",
                    zoom, figure.parts.size(), renderer.draw_calls(), submit_ms / kFrames, total.millis() / kFrames);
    }
}

// Isolate the skinned player model and its tool at gameplay and close-up zoom.
void bench_model(ModelRenderer& models, PlagueAnimator& animator, const IsometricCamera& view,
                 const MapRenderer& renderer, const TileMap& map) {
    RenderTarget target;
    target.create(static_cast<i32>(view.viewport.x), static_cast<i32>(view.viewport.y));
    target.bind();
    glClearColor(0.030f, 0.036f, 0.046f, 1.0f);
    constexpr int kFrames = 30;
    PlagueDraw item;
    item.position = {static_cast<f32>(map.start.x) + 0.5f, static_cast<f32>(map.start.y) + 0.5f};
    item.heading = kPi * 0.5f;
    item.tool = HumanTool::Axe;
    item.show_tool = true;
    HumanMotion motion;
    motion.speed = kJogSpeed;
    motion.phase = 0.3f;
    motion.tool = HumanTool::Axe;
    motion.work = 1.0f;
    motion.work_phase = 0.6f;
    animator.pose(motion);
    for (const f32 zoom : {kFollowZoom, kMaxZoom}) {
        IsometricCamera camera = view;
        camera.zoom = zoom;
        camera.center = item.position;
        camera.center_height = renderer.surface_height(item.position, zoom);
        item.ground_height = camera.center_height;
        target.bind();
        for (int i = 0; i < 3; ++i) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            models.draw(camera, nullptr, 1.0f, item, animator.current());
        }
        glFinish();
        const Stopwatch total;
        f64 submit_ms = 0.0;
        for (int i = 0; i < kFrames; ++i) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            const Stopwatch submit;
            models.draw(camera, nullptr, 1.0f, item, animator.current());
            submit_ms += submit.millis();
            glFinish();
        }
        std::printf("  model %3.0f px/tile  %u calls   submit %7.3f ms   frame %7.3f ms\n",
                    zoom, models.draw_calls(), submit_ms / kFrames, total.millis() / kFrames);
    }
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
    config.visible = options.screenshot == nullptr && options.character_sheet == nullptr && !options.bench && options.record_demo == nullptr;
    config.vsync = options.vsync;
    Window window(config);

    MapRenderer renderer;
    const std::string assets = find_assets(options.assets);
    {
        if (assets.empty()) ZL_LOG("art: no assets directory found; drawing placeholder art in code");
        const Stopwatch timer;
        renderer.init(jobs, assets.empty() ? nullptr : assets.c_str());
        const TerrainArt& art = renderer.art();
        ZL_LOG("art: %s ground, %s sprites, ready in %.1f ms", art.painted_ground ? "painted" : "placeholder",
               art.painted_sprites ? "painted" : "placeholder", timer.millis());
    }

    TileMap map;
    if (config.visible && options.map.size >= 1024) {
        // A multi-kilometre map takes real work. Keep the window responsive
        // while the CPU generator runs, rather than freezing a black window.
        window.set_title("Zillions | Generating the vast wilderness...");
        auto pending = std::async(std::launch::async, [&] { return generate_map(options.map, jobs); });
        while (pending.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
            window.poll();
            const IVec2 fb = window.framebuffer_size();
            if (fb.x <= 0 || fb.y <= 0) continue;
            RenderTarget::bind_default(fb.x, fb.y);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.055f, 0.052f, 0.044f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            const i32 width = fb.x / 3, left = (fb.x - width) / 2;
            glEnable(GL_SCISSOR_TEST);
            glScissor(left, fb.y / 2, width, 5);
            glClearColor(0.16f, 0.15f, 0.11f, 1); glClear(GL_COLOR_BUFFER_BIT);
            const f32 phase = static_cast<f32>(std::fmod(now_seconds() * 0.35, 1.0));
            glScissor(left + static_cast<i32>(phase * static_cast<f32>(width * 3 / 4)), fb.y / 2, width / 4, 5);
            glClearColor(0.58f, 0.56f, 0.35f, 1); glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            window.swap_buffers();
        }
        map = pending.get();
        if (window.should_close()) return 0;
        window.set_title("Zillions | Raising mountains and planting ancient forests...");
    } else map = generate_map(options.map, jobs);
    {
        const Stopwatch timer;
        renderer.set_map(map, jobs);
        ZL_LOG("built relief and scenery in %.1f ms", timer.millis());
    }
    log_map(map);

    FigureRenderer figures;
    figures.init(assets.empty() ? nullptr : assets.c_str());
    figures.set_style(options.character_style);

    // The plague doctor is the player. The procedural figure stays as the
    // missing-model fallback, and as the reference sheet for the painted rig.
    ModelRenderer models;
    PlagueAnimator animator;
    bool plague_ready = false;
    if (options.character) {
        models.init(assets.empty() ? nullptr : assets.c_str());
        plague_ready = models.ready() && animator.init(models.model());
        if (plague_ready) ZL_LOG("the plague doctor is the player");
        else ZL_LOG("model: plague doctor unavailable; using the procedural character");
    }
    Scene scene{renderer, figures, plague_ready ? &models : nullptr, plague_ready ? &animator : nullptr, plague_ready};
    if (options.character_sheet) return save_character_sheet(scene, map, options) ? 0 : 1;

    // The player's character, standing at the colony's start.
    Player player;
    // The plague doctor's body is authored in the GLB; the seeded looks and
    // hood belong to the procedural fallback and would otherwise change the
    // collision body without changing the visible model.
    player.look = plague_ready ? hero_look() : make_look(options.look_seed, options.hood_up);
    const auto place_player = [&](bool posed) {
        if (!(options.have_player && posed ? player.spawn(map, options.player) : player.spawn(map))) ZL_LOG("the map has no open ground for the character");
        if (posed) {
            HumanMotion pose = options.pose;
            pose.time = options.time;
            pose.wade = player.motion().wade;
            player.set_motion(pose);
        }
    };
    place_player(true);
    Survival survival;
    const bool interactive = !options.screenshot && !options.bench && !options.record_demo && !options.character_sheet;
    if (interactive && options.character) { survival.reset(map); renderer.refresh_objects(map); }
    Figure figure;
    // Rebuilds the figure for the moment being drawn; null when there is no character.
    const auto pose_figure = [&](f32 alpha) -> const Figure* {
        if (!options.character || plague_ready) return nullptr;
        HumanMotion pose = player.motion_at(alpha);
        pose.tool = options.tool;
        pose.work = options.tool == HumanTool::None ? 0.0f : 1.0f;
        pose.work_phase = options.work_phase;
        if (survival.active_node() >= 0) {
            const auto& node = survival.nodes[static_cast<size_t>(survival.active_node())];
            const Vec2 direction = map.scenery.objects[node.object].position - player.position();
            pose.heading = std::atan2(direction.y,direction.x);
            pose.tool = survival.held_tool == Item::Axe ? HumanTool::Axe : (survival.held_tool == Item::Pickaxe ? HumanTool::Pickaxe : HumanTool::None);
            pose.work = 1; pose.work_phase = survival.harvest_progress();
        }
        build_human(player.look, pose, figure);
        return &figure;
    };
    // The camera looks at the character's middle rather than its feet.
    const auto player_focus = [&](f32 alpha) { return player.position_at(alpha); };

    IsometricCamera camera;
    camera.rotation = options.rotation;
    camera.viewport = {static_cast<f32>(options.width), static_cast<f32>(options.height)};
    if (options.whole_map) frame_whole_map(camera, map, renderer);
    else frame_start(camera, map, renderer);
    if (options.character && options.have_player) camera.center = player_focus(1.0f);
    if (options.have_center) camera.center = options.center;
    if (options.zoom > 0.0f) camera.zoom = options.zoom;
    if (options.have_center || options.have_player) camera.center_height = renderer.surface_height(camera.center, camera.zoom);

    MapDrawOptions draw;
    draw.time = options.time;
    draw.overlay = options.overlay;
    draw.grid = options.grid;
    draw.shadows = options.shadows;
    draw.collisions = options.collisions;

    if (options.record_demo) return record_player_demo(scene, map, player, camera, draw, options) ? 0 : 1;

    if (options.bench) {
        std::printf("isometric renderer, %dx%d, 3 visible draws plus 2 cached shadow draws\n", options.width, options.height);
        IsometricCamera view = camera;
        frame_start(view, map, renderer);
        bench_view("start", renderer, view);
        view.zoom = 12.0f;
        bench_view("zoomed out", renderer, view);
        frame_whole_map(view, map, renderer);
        bench_view("whole map", renderer, view);

        for (FigureStyle style : {FigureStyle::Painted, FigureStyle::Smooth}) {
            figures.set_style(style);
            std::printf("procedural figure, %s (pose + upload + shader + composite)\n", style == FigureStyle::Painted ? "painted sprite" : "smooth");
            bench_figure(figures, player.look, camera);
        }
        figures.set_style(options.character_style);

        if (plague_ready) {
            std::printf("plague doctor, skinned mesh + axe (pose + upload + shader)\n");
            bench_model(models, animator, camera, renderer, map);
        }

        std::printf("sprite batch, 1 draw call per frame\n");
        SpriteBatch horde;
        horde.init();
        for (const u32 count : {10000u, 100000u, 250000u}) bench_horde(count, renderer, map, jobs, horde, static_cast<const Camera2D&>(view));
        return 0;
    }

    if (options.screenshot) {
        PlagueDraw plague_draw;
        if (plague_ready) {
            HumanMotion motion = player.motion();
            motion.tool = options.tool;
            motion.work = options.tool == HumanTool::None ? 0.0f : 1.0f;
            motion.work_phase = options.work_phase;
            animator.pose(motion);
            const HumanMotion shown = player.motion_at(1.0f);
            plague_draw.position = player.position_at(1.0f);
            plague_draw.ground_height = renderer.surface_height(plague_draw.position, camera.zoom);
            plague_draw.heading = shown.heading;
            plague_draw.wade = shown.wade;
            plague_draw.marker = saturate(1.0f - smoothstep(9.0f, 16.0f, camera.zoom));
            plague_draw.tool = options.tool;
            plague_draw.show_tool = options.tool != HumanTool::None;
        }
        return save_screenshot(scene, camera, draw, pose_figure(1.0f), player.position(), options.screenshot,
                               plague_ready ? &plague_draw : nullptr) ? 0 : 1;
    }

    // --- interactive loop ---
    SurvivalUI hud;
    hud.init(assets.empty() ? nullptr : assets.c_str());
    u32 rendered_revision = survival.world_revision;
    float impact_age = 10;
    Vec2 impact_position;
    Item impact_item = Item::None;
    FixedStep sim(60.0);
    f64 last = now_seconds(), title_timer = 0.0, frame_ms_sum = 0.0;
    int frames = 0, shot_index = 0;
    // The camera follows the character until the player switches to management
    // mode with Space.
    bool follow = options.character && !options.whole_map && !options.have_center;
    f32 saved_follow_zoom = options.zoom > 0.0f ? options.zoom : kFollowZoom;
    f32 saved_management_zoom = kManagementZoom;
    CameraModeSwitch mode_switch;
    PlayerAutonomy autonomy;
    u64 autonomy_switches = 0;
    autonomy.reset(player.position(), map.seed);
    u64 look_seed = options.look_seed;
    if (follow) {
        camera.center = player_focus(1.0f);
        if (options.zoom <= 0.0f) camera.zoom = kFollowZoom;
        camera.center_height = renderer.surface_height(camera.center, camera.zoom) + 0.5f;
    }

    RenderTarget frame;
    while (!window.should_close()) {
        window.poll();
        const Input& in = window.input();
        const f64 now = now_seconds();
        const f64 dt = now - last;
        last = now;
        impact_age += static_cast<float>(dt);

        const IVec2 fb = window.framebuffer_size();
        if (fb.x <= 0 || fb.y <= 0) continue;  // minimised
        camera.viewport = {static_cast<f32>(fb.x), static_cast<f32>(fb.y)};
        const f32 min_zoom = fit_zoom(map, camera.viewport) * 0.7f;
        const f32 max_zoom = options.character ? kMaxZoom : 128.0f;

        if (in.pressed(Key::Escape)) {
            if (hud.placing) hud.placing = false;
            else if (hud.open) hud.close(survival);
            else window.request_close();
        }
        if (options.character && (in.pressed(Key::I) || in.pressed(Key::B))) {
            survival.cancel_harvest();
            if (in.pressed(Key::B) && hud.open && hud.chest >= 0) { if (hud.close(survival)) hud.open = true; }
            else if (hud.open) hud.close(survival);
            else { hud.open = true; hud.chest = -1; hud.placing = false; }
        }
        if (options.character) hud.update(in, camera.viewport, survival);
        if (hud.placing && !follow) {
            hud.placing=false;
            survival.message="Press Space to return to your character before placing a chest";
            survival.message_time=4;
        }
        // Reset is a debug operation once play has a persistent inventory.
        if (in.pressed(Key::R) && (!options.character || in.down(Key::LeftControl)) && !hud.hand.count) {
            options.map.seed += 1;
            map = generate_map(options.map, jobs);
            if (options.character) { survival.reset(map); hud.close(survival); hud.placing = false; }
            renderer.set_map(map, jobs);
            log_map(map);
            place_player(false);
            autonomy.reset(player.position(), map.seed);
            mode_switch.cancel();
            if (follow) {
                camera.center = player_focus(1.0f);
                camera.center_height = renderer.surface_height(camera.center, camera.zoom) + 0.5f;
            }
        }
        if (in.pressed(Key::Q) || in.pressed(Key::E)) {
            Vec2 pivot;
            if (!follow && renderer.pick(camera, camera.viewport * 0.5f, pivot)) {
                camera.center = pivot;
                camera.center_height = renderer.surface_height(pivot, camera.zoom);
            }
            mode_switch.cancel();
            camera.turn(in.pressed(Key::E) ? 1 : -1);
        }
        if (in.pressed(Key::G)) draw.grid = !draw.grid;
        if (in.pressed(Key::F3)) draw.collisions = !draw.collisions;
        if (in.pressed(Key::V)) draw.overlay = static_cast<MapOverlay>((static_cast<int>(draw.overlay) + 1) % static_cast<int>(MapOverlay::Count));
        if (in.pressed(Key::F) && !follow) {
            frame_whole_map(camera, map, renderer);
            mode_switch.cancel();
        }
        if (in.pressed(Key::H)) {
            mode_switch.cancel();
            if (options.character) {
                camera.center = player_focus(static_cast<f32>(sim.alpha()));
                camera.center_height = renderer.surface_height(camera.center, camera.zoom) + 0.5f;
                if (follow) saved_follow_zoom = camera.zoom = kFollowZoom;
            } else {
                frame_start(camera, map, renderer);
            }
        }
        if (options.character) {
            if (in.pressed(Key::Space) && !hud.open && !hud.placing) {
                survival.cancel_harvest();
                follow = !follow;
                const Vec2 focus = player_focus(static_cast<f32>(sim.alpha()));
                if (!follow) {
                    saved_follow_zoom = clamp(camera.zoom, min_zoom, kMaxZoom);
                    const f32 target_zoom = clamp(saved_management_zoom, min_zoom, kMaxZoom);
                    const f32 heading = player.motion().heading;
                    const Vec2 look_dir{std::cos(heading), std::sin(heading)};
                    mode_switch.begin_to_management(camera, focus, look_dir, target_zoom);
                    autonomy.reset(player.position(), map.seed ^ (++autonomy_switches * 0x9E3779B97F4A7C15ull));
                } else {
                    if (camera.zoom > min_zoom * 1.2f) saved_management_zoom = clamp(camera.zoom, min_zoom, kMaxZoom);
                    const f32 target_zoom = clamp(saved_follow_zoom, min_zoom, kMaxZoom);
                    mode_switch.begin_to_character(camera, target_zoom);
                }
            }
            if (!plague_ready) {
                if (in.pressed(Key::C)) player.look = make_look(++look_seed, false);
                if (in.pressed(Key::Num1)) player.look = make_look(look_seed = 0, false);
                if (in.pressed(Key::X)) player.look.hood_up = !player.look.hood_up;
                if (in.pressed(Key::F6)) figures.set_style(figures.style() == FigureStyle::Painted ? FigureStyle::Smooth : FigureStyle::Painted);
            }
        }
        if (in.pressed(Key::F5)) {
            const std::string reload_assets = find_assets(options.assets);
            const Stopwatch timer;
            renderer.reload_art(jobs, reload_assets.empty() ? nullptr : reload_assets.c_str());
            figures.reload_art(reload_assets.empty()?nullptr:reload_assets.c_str());
            hud.reload_art(reload_assets.empty()?nullptr:reload_assets.c_str());
            const TerrainArt& art = renderer.art();
            ZL_LOG("art reloaded: %s ground, %s sprites, ready in %.1f ms", art.painted_ground ? "painted" : "placeholder",
                   art.painted_sprites ? "painted" : "placeholder", timer.millis());
        }

        // The direction keys steer the character while the camera follows it.
        // In management mode (or without a character) the camera behaves like a
        // city builder: cursor near the window borders, WASD/arrows or
        // right/middle-drag all pan across the map.
        Vec2 keys{0, 0};
        if (in.down(Key::A) || in.down(Key::Left)) keys.x -= 1.0f;
        if (in.down(Key::D) || in.down(Key::Right)) keys.x += 1.0f;
        if (in.down(Key::W) || in.down(Key::Up)) keys.y -= 1.0f;
        if (in.down(Key::S) || in.down(Key::Down)) keys.y += 1.0f;
        PlayerInput input;
        Vec2 pan_delta{0, 0};
        if (follow && !hud.open) {
            input.move = camera.screen_delta_to_world(keys * camera.zoom);
            input.sprint = in.down(Key::LeftShift);
            input.walk = in.down(Key::LeftControl);
        } else if (!follow && !hud.open) {
            const bool dragging = in.down(MouseButton::Right) || in.down(MouseButton::Middle);
            const Vec2 border = dragging ? Vec2{0, 0} : edge_pan(in.mouse, camera.viewport, in.mouse_inside, kEdgePanMargin);
            Vec2 pan = keys + border;
            const f32 pan_len = length(pan);
            if (pan_len > 1.0f) pan = pan / pan_len;
            const f32 speed = in.down(Key::LeftShift) ? 2400.0f : 1000.0f;
            pan_delta = camera.screen_delta_to_world(pan * (speed * static_cast<f32>(dt)));
            if (dragging) pan_delta -= camera.screen_delta_to_world(in.mouse_delta);
            camera.center += pan_delta;
        }

        Vec2 cursor_world;
        const bool world_hit = renderer.pick(camera, in.mouse, cursor_world);
        const bool world_input = options.character && follow && in.mouse_inside && world_hit && !hud.blocks_world(in.mouse,camera.viewport);
        int hovered_node = world_input && !hud.placing ? survival.pick_node(map, cursor_world) : -1;
        int hovered_chest = -1;
        if (world_input && !hud.placing) {
            for (int c = 0; c < static_cast<int>(survival.chests.size()); ++c)
                if (length(map.scenery.objects[survival.chests[static_cast<size_t>(c)].object].position-cursor_world)<0.8f) hovered_chest=c;
        }
        if (follow && !hud.open && hud.placing) {
            if (in.pressed(MouseButton::Right)) hud.placing = false;
            else if (in.pressed(MouseButton::Left) && world_input && survival.place_chest(map,player.position(),cursor_world,hud.placement_facing)) hud.placing=false;
        } else if (follow && hovered_chest >= 0 && in.pressed(MouseButton::Left)) {
            if (survival.in_reach(map,player.position(),survival.chests[static_cast<size_t>(hovered_chest)].object)) {
                hud.chest=hovered_chest;hud.open=true;survival.cancel_harvest();
            } else {survival.message="Move closer to the camp chest";survival.message_time=3;}
        }

        // The simulation ticks at a fixed rate; in management mode the
        // character wanders the local area autonomously.
        const int steps = sim.advance(dt);
        if (options.character) {
            const f32 step_dt = static_cast<f32>(sim.step_seconds());
            for (int k = 0; k < steps; ++k) {
                const PlayerInput step_input = hud.open ? PlayerInput{} : (follow ? input : autonomy.step(map, player, step_dt));
                player.step(map, step_input, step_dt);
                const int remaining = hovered_node >= 0 ? survival.nodes[static_cast<size_t>(hovered_node)].remaining : 0;
                survival.harvest(map, player.position(), hovered_node,
                    follow && !hud.open && !hud.placing && in.down(MouseButton::Left),
                    length(step_input.move)>0 || length(player.velocity())>0.1f, step_dt);
                if (hovered_node >= 0 && survival.nodes[static_cast<size_t>(hovered_node)].remaining < remaining) {
                    const auto& node=survival.nodes[static_cast<size_t>(hovered_node)];
                    impact_age=0;impact_position=map.scenery.objects[node.object].position;impact_item=node.item;
                }
                survival.step(step_dt);
                if (plague_ready) {
                    HumanMotion m = player.motion();
                    m.speed = model_ground_speed(player);
                    if (survival.active_node() >= 0) {
                        const auto& node = survival.nodes[static_cast<size_t>(survival.active_node())];
                        const Vec2 direction = map.scenery.objects[node.object].position - player.position();
                        m.heading = std::atan2(direction.y, direction.x);
                        m.tool = survival.held_tool == Item::Axe ? HumanTool::Axe
                               : (survival.held_tool == Item::Pickaxe ? HumanTool::Pickaxe : HumanTool::None);
                        m.work = 1;
                        m.work_phase = survival.harvest_progress();
                    } else {
                        m.tool = HumanTool::None;
                        m.work = 0;
                        m.work_phase = 0;
                    }
                    animator.update(m, step_dt);
                }
            }
        }
        const f32 alpha = static_cast<f32>(sim.alpha());
        draw.time = static_cast<f32>(now);
        if (survival.world_revision != rendered_revision) {
            renderer.refresh_objects(map); rendered_revision=survival.world_revision;
        }

        // The wheel zooms about the cursor in management mode, or about the
        // character while following.
        const Vec2 focus = player_focus(alpha);
        if (in.scroll != 0.0f) {
            mode_switch.cancel_zoom();
            if (follow) {
                camera.zoom = clamp(camera.zoom * std::pow(1.15f, in.scroll), min_zoom, kMaxZoom);
                saved_follow_zoom = camera.zoom;
            } else {
                Vec2 anchor;
                const bool hit = renderer.pick(camera, in.mouse, anchor);
                const f32 height = hit ? renderer.surface_height(anchor, camera.zoom) : camera.center_height;
                if (!hit) anchor = camera.screen_to_world(in.mouse, height);
                camera.zoom = clamp(camera.zoom * std::pow(1.15f, in.scroll), min_zoom, max_zoom);
                camera.center += anchor - camera.screen_to_world(in.mouse, hit ? renderer.surface_height(anchor, camera.zoom) : height);
                saved_management_zoom = camera.zoom;
            }
        }
        const f32 switch_pulse = mode_switch.step(camera, focus, pan_delta, static_cast<f32>(dt), min_zoom, max_zoom);
        if (follow && !mode_switch.active) {
            camera.center += (focus - camera.center) * (1.0f - std::exp(-static_cast<f32>(dt) * 9.0f));
        }
        if (follow || mode_switch.active) {
            const f32 target_height = renderer.surface_height(camera.center, camera.zoom) + (follow ? 0.5f : 0.0f);
            camera.center_height += (target_height - camera.center_height) * (1.0f - std::exp(-static_cast<f32>(dt) * 9.0f));
        }
        camera.center.x = clamp(camera.center.x, 0.0f, static_cast<f32>(map.width));
        camera.center.y = clamp(camera.center.y, 0.0f, static_cast<f32>(map.height));

        const Stopwatch frame_timer;
        if (frame.width() != fb.x || frame.height() != fb.y) frame.create(fb.x, fb.y, 4);
        frame.bind();
        const Figure* posed = pose_figure(alpha);
        const Vec2 standing = player.position_at(alpha);
        const f32 management_ring = !follow ? (1.0f - smoothstep(26.0f, 44.0f, camera.zoom)) * 0.32f : 0.0f;
        PlagueDraw plague_draw;
        if (plague_ready) {
            const HumanMotion motion = player.motion_at(alpha);
            const bool gathering = survival.active_node() >= 0;
            f32 heading = motion.heading;
            HumanTool tool = HumanTool::None;
            if (gathering) {
                const auto& node = survival.nodes[static_cast<size_t>(survival.active_node())];
                const Vec2 direction = map.scenery.objects[node.object].position - player.position();
                heading = std::atan2(direction.y, direction.x);
                tool = survival.held_tool == Item::Axe ? HumanTool::Axe
                     : (survival.held_tool == Item::Pickaxe ? HumanTool::Pickaxe : HumanTool::None);
            }
            plague_draw.position = standing;
            plague_draw.ground_height = renderer.surface_height(standing, camera.zoom);
            plague_draw.heading = heading;
            plague_draw.wade = motion.wade;
            plague_draw.marker = saturate(std::max(1.0f - smoothstep(9.0f, 16.0f, camera.zoom), std::max(switch_pulse, management_ring)));
            plague_draw.tool = tool;
            plague_draw.show_tool = gathering;
        }
        scene.draw(camera, draw, posed, standing, std::max(switch_pulse, management_ring),
                   plague_ready ? &plague_draw : nullptr);
        if (options.character) {
            std::string prompt;
            if (hovered_node>=0) {
                const auto& node=survival.nodes[static_cast<size_t>(hovered_node)];
                const char* problem=survival.harvest_problem(map,player.position(),hovered_node);
                prompt=std::string(item_name(node.item))+" / "+std::to_string(node.remaining)+" left / "+(problem?problem:"Hold left mouse to gather");
                const Vec2 p=map.scenery.objects[node.object].position;
                hud.marker(camera.world_to_screen(p,renderer.surface_height(p,camera.zoom)),camera.viewport,!problem,survival.harvest_progress());
            } else if (hovered_chest>=0) prompt="CAMP CHEST / Click nearby to open storage";
            if (hud.placing && world_hit) {
                const Vec2 p{std::floor(cursor_world.x)+0.5f,std::floor(cursor_world.y)+0.5f};
                const bool valid=survival.can_place_chest(map,player.position(),p);
                renderer.draw_chest_preview(camera,p,valid,hud.placement_facing);
                hud.marker(camera.world_to_screen(p,renderer.surface_height(p,camera.zoom)),camera.viewport,valid);
            }
            hud.impact(camera.world_to_screen(impact_position,renderer.surface_height(impact_position,camera.zoom)),camera.viewport,impact_item,impact_age);
            hud.draw(camera.viewport,in,survival,prompt.empty()?nullptr:prompt.c_str(),follow);
        }
        frame.present();
        frame_ms_sum += frame_timer.millis();
        ++frames;

        if (in.pressed(Key::F2)) {
            const std::string path = "zillions_" + std::to_string(map.seed) + "_" + std::to_string(shot_index++) + ".png";
            const Image capture = frame.read_pixels();
            if(write_png(path.c_str(),capture)) ZL_LOG("wrote %s",path.c_str());
        }

        window.swap_buffers();

        title_timer += dt;
        if (title_timer >= 0.5) {
            char title[200];
            std::snprintf(title, sizeof(title), "Zillions | seed %llu | %dx%d | %.0f fps | %.2f ms CPU/frame | %u sprites | isometric %d deg | Q/E rotate%s%s",
                          static_cast<unsigned long long>(map.seed), map.width, map.height, frames / title_timer, frame_ms_sum / frames,
                          renderer.sprites_drawn(), camera.rotation * 90,
                          options.character ? (plague_ready ? " | plague doctor" : (figures.style() == FigureStyle::Painted ? " | painted sprite" : " | smooth")) : "",
                          options.character && !follow ? " | management mode (Space)" : "");
            window.set_title(title);
            title_timer = 0.0;
            frame_ms_sum = 0.0;
            frames = 0;
        }
    }
    return 0;
}
