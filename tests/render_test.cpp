// GPU regression checks. Run with a display, e.g. xvfb-run -a ./build/tests/zl_render_tests.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/jobs.h"
#include "actor/human.h"
#include "actor/player.h"
#include "gfx/gl.h"
#include "gfx/render_target.h"
#include "platform/window.h"
#include "render/map_renderer.h"
#include "render/figure_renderer.h"

using namespace zl;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

bool same_image(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height) return false;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const Color x = a.pixels[i], y = b.pixels[i];
        if (x.r != y.r || x.g != y.g || x.b != y.b) return false;
    }
    return true;
}

TileMap fixture(i32 resolution) {
    TileMap map;
    // Deliberately rectangular, with partial chunks on both boundaries.
    map.width = 45;
    map.height = 63;
    map.resolution = resolution;
    map.seed = 7;
    const size_t n = static_cast<size_t>(map.tile_count());
    map.terrain.assign(n, Terrain::Dirt);
    map.resource.assign(n, Resource::None);
    map.level.assign(n, 0);
    map.flags.assign(n, TileFlag::Walkable | TileFlag::Buildable);
    map.elevation.assign(n, 0.1f);
    map.moisture.assign(n, 0.5f);
    map.shore_dist.assign(n, 0);
    for (i32 y = 20; y < 43; ++y) {
        for (i32 x = 15; x < 30; ++x) map.level[static_cast<size_t>(map.index(x, y))] = 2;
    }
    map.terrain[static_cast<size_t>(map.index(23, 31))] = Terrain::Forest;
    return map;
}

void check_sprite_depth() {
    RenderTarget target;
    target.create(100, 100);
    target.bind();
    Texture atlas;
    atlas.create(Image(4, 4, {255, 0, 0, 255}), TextureFilter::Nearest, TextureWrap::Clamp);
    SpriteBatch batch;
    batch.init();
    Camera2D camera;
    camera.viewport = {100, 100};
    camera.center = {50, 50};
    camera.zoom = 1;
    const auto render = [&](f32 base, f32 upright, f32 ground, f32 anchor) {
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClearDepth(0.5);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        SpriteInstance* out = batch.begin(1);
        *out = {10, 10, 80, 80, 0, 0, 65535, 65535, 0xFFFFFFFFu};
        out->depth = base;
        out->depth_span = upright;
        out->ground_span = ground;
        out->anchor = anchor;
        batch.draw(1, camera, atlas);
        return target.read_pixels();
    };
    const Image hidden = render(0.2f, 0, 0, 0);
    CHECK(hidden.at(50, 50).r == 0);
    const Image visible = render(-0.2f, 0, 0, 0);
    CHECK(visible.at(50, 50).r == 255);
    // A cliff hides the trunk while the crown extends above its silhouette.
    const Image upright = render(0.2f, 0.8f, 0, 1);
    CHECK(upright.at(50, 20).r == 255);
    CHECK(upright.at(50, 80).r == 0);
    // The contact shadow lies on the ground, with the opposite depth slope.
    const Image shadow = render(0.2f, 0.8f, -0.8f, 0.5f);
    CHECK(shadow.at(50, 20).r == 255);
    CHECK(shadow.at(50, 50).r == 0);
    CHECK(shadow.at(50, 80).r == 255);
    glClearDepth(1);
    glDisable(GL_DEPTH_TEST);
}

void check_shadows(MapRenderer& renderer, JobSystem& jobs) {
    RenderTarget target;
    target.create(640, 480);
    IsometricCamera camera;
    camera.viewport = {640, 480};
    camera.center = {23.5f, 31.5f};
    camera.zoom = 24;
    TileMap map = fixture(1);
    std::fill(map.level.begin(), map.level.end(), 0);
    std::fill(map.terrain.begin(), map.terrain.end(), Terrain::Dirt);
    map.scenery.ready = true;
    map.scenery.objects.clear();
    renderer.set_map(map, jobs);
    camera.center_height = renderer.surface_height(camera.center, camera.zoom);
    const auto render = [&](bool shadows) {
        target.bind();
        MapDrawOptions options;
        options.shadows = shadows;
        renderer.draw(camera, options);
        return target.read_pixels();
    };
    // Flat ground without any caster stays fully lit: no acne,
    // rectangular sprite-canvas shadows or stray texture-border shadows.
    const Image empty_lit = render(false), empty_shaded = render(true);
    if (!same_image(empty_lit, empty_shaded)) {
        write_png("/tmp/shadow-empty-lit.png", empty_lit);
        write_png("/tmp/shadow-empty-shaded.png", empty_shaded);
        i32 changes = 0, maximum = 0;
        for (size_t i = 0; i < empty_lit.pixels.size(); ++i) {
            const i32 delta = std::abs(static_cast<i32>(empty_lit.pixels[i].r) - empty_shaded.pixels[i].r);
            if (delta) ++changes;
            maximum = std::max(maximum, delta);
        }
        std::printf("flat shadow difference: %d pixels, maximum %d\n", changes, maximum);
    }
    CHECK(same_image(empty_lit, empty_shaded));

    map.terrain[static_cast<size_t>(map.index(23, 31))] = Terrain::Forest;
    map.resource[static_cast<size_t>(map.index(29, 31))] = Resource::Stone;
    map.scenery.ready = false;
    renderer.set_map(map, jobs);
    for (i32 rotation = 0; rotation < 4; ++rotation) {
        camera.rotation = rotation;
        const Image lit = render(false), shaded = render(true);
        i32 darkened = 0, brightened = 0;
        for (size_t i = 0; i < lit.pixels.size(); ++i) {
            if (static_cast<i32>(lit.pixels[i].r) - shaded.pixels[i].r > 5) ++darkened;
            if (static_cast<i32>(shaded.pixels[i].r) - lit.pixels[i].r > 2) ++brightened;
        }
        CHECK(darkened > 100);
        CHECK(brightened == 0);
        // Repeat and toggle without changing the view: cached depth and
        // receiver bindings must reproduce exactly the same shadow image.
        CHECK(same_image(shaded, render(true)));
        render(false);
        CHECK(same_image(shaded, render(true)));
        if (rotation == 0) {
            i32 ground_shadow = 0;
            // Sample down-sun ground below the tree's painted contact shadow.
            // This verifies a real displaced cast shadow outside its canvas.
            for (f32 y = 32.0f; y < 33.0f; y += 0.1f) {
                for (f32 x = 24.2f; x < 26.1f; x += 0.1f) {
                    const Vec2 pixel = camera.world_to_screen({x, y}, renderer.surface_height({x, y}, camera.zoom));
                    const i32 px = static_cast<i32>(pixel.x), py = static_cast<i32>(pixel.y);
                    if (px >= 0 && py >= 0 && px < 640 && py < 480 &&
                        static_cast<i32>(lit.at(px, py).r) - shaded.at(px, py).r > 5) ++ground_shadow;
                }
            }
            CHECK(ground_shadow > 10);
        }
    }

    // A cliff above the viewport can still shade receivers in view. Cast
    // selection must use the light volume, rather than the camera's culling.
    map = fixture(1);
    std::fill(map.terrain.begin(), map.terrain.end(), Terrain::Dirt);
    renderer.set_map(map, jobs);
    camera.rotation = 0;
    camera.center = {32, 32};
    camera.zoom = 128;
    camera.center_height = renderer.surface_height(camera.center, camera.zoom);
    const Image lit = render(false), shaded = render(true);
    i32 cliff_shadow = 0;
    for (size_t i = 0; i < lit.pixels.size(); ++i) {
        if (static_cast<i32>(lit.pixels[i].r) - shaded.pixels[i].r > 5) ++cliff_shadow;
    }
    CHECK(cliff_shadow > 300);
    CHECK(glGetError() == GL_NO_ERROR);
}

void check_world(MapRenderer& renderer, JobSystem& jobs) {
    RenderTarget target;
    target.create(640, 480);
    IsometricCamera camera;
    camera.viewport = {640, 480};
    MapDrawOptions options;
    for (i32 resolution : {1, 2, 4}) {
        const TileMap map = fixture(resolution);
        renderer.set_map(map, jobs);
        for (i32 view = 0; view < 4; ++view) {
            camera.rotation = view;
            camera.center = {22.5f, 31.5f};
            camera.center_height = renderer.ground_height(camera.center);
            for (f32 zoom : {4.0f, 16.0f, 64.0f}) {
                camera.zoom = zoom;
                target.bind();
                renderer.draw(camera, options);
                // Raised plateau stays at the centre, and CPU picking agrees
                // with the GPU's depth at the exact same projected position.
                Vec2 picked;
                CHECK(renderer.pick(camera, camera.viewport * 0.5f, picked));
                CHECK(length(picked - camera.center) < 0.001f);
                f32 depth = 0;
                glReadPixels(320, 239, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
                CHECK(depth > 0 && depth < 1);
                CHECK(renderer.ground_height(camera.center) > renderer.ground_height({5, 5}) + 2.0f * static_cast<f32>(resolution));
                CHECK(!renderer.pick(camera, {-100000, -100000}, picked));
            }
            renderer.frame_map(camera);
            target.bind();
            renderer.draw(camera, options);
            const Image framed = target.read_pixels();
            // F fits all geometry, including partial edge chunks and skirt.
            for (i32 x = 0; x < framed.width; ++x) {
                CHECK(framed.at(x, 0).r < 12 && framed.at(x, 479).r < 12);
            }
            for (i32 y = 0; y < framed.height; ++y) {
                CHECK(framed.at(0, y).r < 12 && framed.at(639, y).r < 12);
            }
            // Four quarter turns return bit-for-bit to the same picture.
            for (i32 i = 0; i < 4; ++i) camera.turn(1);
            renderer.draw(camera, options);
            CHECK(same_image(framed, target.read_pixels()));
        }
    }

    // Match the live game's multisample world target and presentation path.
    // Compare the actual resolved world against what reaches the window.
    camera.rotation = 0;
    renderer.frame_map(camera);
    RenderTarget frame;
    frame.create(640, 480, 4);
    frame.bind();
    GLint bits = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
    CHECK(bits >= 24);
    renderer.draw(camera, options);
    const Image offscreen = frame.read_pixels();
    frame.present();
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
    CHECK(bits >= 24);
    Image live(640, 480);
    glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, live.bytes());
    for (i32 y = 0; y < 240; ++y) {
        std::swap_ranges(live.pixels.begin() + y * 640, live.pixels.begin() + (y + 1) * 640, live.pixels.begin() + (479 - y) * 640);
    }
    // Presentation copies the resolved colour without reshading foliage.
    i32 different = 0, largest = 0;
    for (size_t i = 0; i < live.pixels.size(); ++i) {
        const i32 delta = std::max({std::abs(static_cast<i32>(live.pixels[i].r) - offscreen.pixels[i].r),
                                   std::abs(static_cast<i32>(live.pixels[i].g) - offscreen.pixels[i].g),
                                   std::abs(static_cast<i32>(live.pixels[i].b) - offscreen.pixels[i].b)});
        if (delta) ++different;
        largest = std::max(largest, delta);
    }
    CHECK(largest <= 4);
    CHECK(different < live.width * live.height / 1000);

    // Both attachments must survive framebuffer resizing.
    target.create(321, 233);
    target.bind();
    camera.viewport = {321, 233};
    renderer.frame_map(camera);
    options.grid = true;
    options.overlay = MapOverlay::Height;
    renderer.draw(camera, options);
    const Image resized = target.read_pixels();
    CHECK(resized.width == 321 && resized.height == 233);

    // F5 must refresh the silhouette depth as well as the visible atlas. An
    // in-place art reload must match a fresh rebuild with the replacement art.
    renderer.reload_art(jobs, renderer.art().painted_sprites ? nullptr : ZL_ASSET_DIR);
    target.bind();
    renderer.draw(camera, options);
    const Image reloaded = target.read_pixels();
    renderer.set_map(fixture(4), jobs);
    target.bind();
    renderer.draw(camera, options);
    CHECK(same_image(reloaded, target.read_pixels()));
    CHECK(glGetError() == GL_NO_ERROR);
}

void check_isometric_figures(MapRenderer& renderer, JobSystem& jobs) {
    TileMap map = fixture(1);
    std::fill(map.level.begin(), map.level.end(), 0);
    std::fill(map.terrain.begin(), map.terrain.end(), Terrain::Dirt);
    renderer.set_map(map, jobs);
    Figure figure;
    build_human(hero_look(), HumanMotion{}, figure);
    FigureRenderer figures;
    figures.init();
    RenderTarget target;
    target.create(640, 480);
    IsometricCamera camera;
    camera.viewport = {640, 480};
    camera.center = {23.5f, 31.5f};
    camera.zoom = 96;
    camera.center_height = renderer.surface_height(camera.center, camera.zoom) + 0.5f;
    for (const FigureStyle style : {FigureStyle::Painted, FigureStyle::Smooth}) {
        figures.set_style(style);
        for (i32 rotation = 0; rotation < 4; ++rotation) {
            camera.rotation = rotation;
            target.bind();
            renderer.draw_ground(camera, MapDrawOptions{});
            const Image ground = target.read_pixels();
            const auto draw = [&](f32 height) {
                target.bind();
                renderer.draw_ground(camera, MapDrawOptions{});
                glDepthMask(GL_FALSE);
                const FigureDraw item{&figure, camera.center, 0, 0, height};
                figures.draw(&item, 1, camera.plane_camera(), &camera, renderer.shadow_view(), renderer.depth_scale());
                glDepthMask(GL_TRUE);
                return target.read_pixels();
            };
            const f32 height = renderer.surface_height(camera.center, camera.zoom);
            const Image visible = draw(height);
            i32 changed = 0;
            for (size_t i = 0; i < ground.pixels.size(); ++i) {
                if (std::abs(static_cast<i32>(ground.pixels[i].r) - visible.pixels[i].r) > 4) ++changed;
            }
            CHECK(changed > 200);
            // Both live painted atlases and smooth figures compare their
            // actual world depth with terrain instead of drawing over it.
            CHECK(same_image(ground, draw(height - 5.0f)));
        }
    }
    glDisable(GL_DEPTH_TEST);
    CHECK(glGetError() == GL_NO_ERROR);
}

void check_collision_rendering(MapRenderer& renderer, JobSystem& jobs) {
    TileMap map = fixture(1);
    std::fill(map.level.begin(), map.level.end(), 0);
    std::fill(map.terrain.begin(), map.terrain.end(), Terrain::Dirt);
    map.terrain[static_cast<size_t>(map.index(23, 31))] = Terrain::Forest;
    map.terrain[static_cast<size_t>(map.index(27, 34))] = Terrain::Rocks;
    map.resource[static_cast<size_t>(map.index(29, 31))] = Resource::Stone;
    RenderTarget target;
    target.create(800, 600);
    IsometricCamera camera;
    camera.viewport = {800, 600};
    camera.center = {23.5f, 31.5f};
    camera.zoom = 96;
    // Populating simulation scenery must not shift or duplicate anything that
    // the viewer previously generated internally for a hand-authored map.
    renderer.set_map(map, jobs);
    camera.center_height = renderer.surface_height(camera.center, camera.zoom);
    target.bind(); renderer.draw(camera, MapDrawOptions{});
    const Image before = target.read_pixels();
    map.scenery = build_scenery(map);
    renderer.set_map(map, jobs);
    target.bind(); renderer.draw(camera, MapDrawOptions{});
    CHECK(same_image(before, target.read_pixels()));
    CHECK(renderer.decor_count() == map.scenery.objects.size());

    FigureRenderer figures;
    figures.init();
    const char* capture = std::getenv("ZL_COLLISION_CAPTURE_DIR");
    if (capture) std::filesystem::create_directories(capture);
    bool seen_tree = false, seen_rock = false, seen_ore = false;
    for (const ObjectCollider& collider : map.scenery.colliders) {
        const SceneryObject& object = map.scenery.objects[collider.object];
        const bool tree = object.kind == SceneryKind::Broadleaf || object.kind == SceneryKind::Conifer;
        const bool rock = object.kind == SceneryKind::Boulder;
        if ((tree && seen_tree) || (rock && seen_rock) || (!tree && !rock && seen_ore)) continue;
        if (tree) seen_tree = true; else if (rock) seen_rock = true; else seen_ore = true;
        Player player;
        const Vec2 axis = length(collider.b - collider.a) > 0.01f
            ? (collider.b - collider.a) / length(collider.b - collider.a) : Vec2{1, 0};
        CHECK(player.spawn(map, object.position - axis * 3.0f));
        // A larger rotated capsule first allows tangential sliding. Keep
        // steering at its centre until that contact has fully settled.
        for (i32 tick = 0; tick < 600; ++tick) {
            player.step(map, {object.position - player.position(), true, false}, 1.0f / 60.0f);
            CHECK(can_stand(map, player.position(), player.body_radius(), player.body_height()));
        }
        // Rounded rotated rocks can slide the body to another face. Verify
        // contact with the physical capsule, rather than a screen-side sign.
        CHECK(length(player.position() - object.position) < collider.radius +
              length(collider.b - collider.a) * 0.5f + player.body_radius() + 0.02f);
        if(player.motion().speed>=0.01f)
            std::printf("contact %s: speed %.6f, player %.4f %.4f, object %.4f %.4f\n",scenery_model(object.kind).name,
                        player.motion().speed,player.position().x,player.position().y,object.position.x,object.position.y);
        CHECK(player.motion().speed < 0.01f);
        Figure figure;
        build_human(player.look, player.motion(), figure);
        for (i32 rotation = 0; rotation < 4; ++rotation) {
            camera.rotation = rotation;
            camera.center = object.position;
            camera.center_height = renderer.surface_height(object.position, camera.zoom) + 0.8f;
            target.bind();
            MapDrawOptions options;
            renderer.draw_ground(camera, options);
            const Vec2 depth = camera.rotate(player.position());
            renderer.draw_scenery(camera, -1.0e30f, depth.x + depth.y);
            const FigureDraw item{&figure, player.position(), 0, 0, renderer.surface_height(player.position(), camera.zoom)};
            glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
            figures.draw(&item, 1, camera.plane_camera(), &camera, renderer.shadow_view(), renderer.depth_scale());
            glDepthMask(GL_TRUE);
            const Vec2 standing = camera.project(player.position(), item.height);
            SeeThrough reveal{{1.0e9f, 1.0e9f}, {-1.0e9f, -1.0e9f}};
            for (const FigurePart& part : figure.parts) for (const Vec3 p : {part.a, part.b}) {
                const Vec2 q = standing + camera.project({p.x, p.y}, p.z - figure.waterline);
                const f32 radius = std::max(part.ra, part.rb);
                reveal.lo.x = std::min(reveal.lo.x, q.x - radius); reveal.lo.y = std::min(reveal.lo.y, q.y - radius);
                reveal.hi.x = std::max(reveal.hi.x, q.x + radius); reveal.hi.y = std::max(reveal.hi.y, q.y + radius);
            }
            renderer.draw_scenery(camera, depth.x + depth.y, 1.0e30f, &reveal);
            renderer.draw_collisions(camera);
            const Image image = target.read_pixels();
            i32 wires = 0;
            for (const Color color : image.pixels) if (color.g > 180 && color.b > 180 && color.r < 100) ++wires;
            CHECK(wires > 30);
            if (capture) {
                const std::string name = tree ? "tree" : (rock ? "rock" : "ore");
                const std::string path = (std::filesystem::path(capture) / ("collision-" + name + "-" + std::to_string(rotation * 90) + ".png")).string();
                CHECK(write_png(path.c_str(), image));
            }
        }
    }
    CHECK(seen_tree && seen_rock && seen_ore);
    CHECK(glGetError() == GL_NO_ERROR);
}
}  // namespace

int main() {
    WindowConfig config;
    config.width = 640;
    config.height = 480;
    config.visible = false;
    Window window(config);
    check_sprite_depth();
    JobSystem jobs(4);
    MapRenderer renderer;
    renderer.init(jobs, ZL_ASSET_DIR);
    check_collision_rendering(renderer, jobs);
    check_shadows(renderer, jobs);
    check_world(renderer, jobs);
    check_isometric_figures(renderer, jobs);
    renderer.reload_art(jobs, nullptr);
    check_shadows(renderer, jobs);
    check_world(renderer, jobs);
    std::printf("isometric GPU checks: %d failures (painted and fallback art)\n", failures);
    return failures ? 1 : 0;
}
