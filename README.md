# Zillions

A colony-survival strategy game in the spirit of *They Are Billions*, written in
C++20 on a small custom engine. This first milestone is the engine core and the
procedural map generator.

![A generated map](docs/screenshots/map-seed-7.jpg)

| | |
|---|---|
| ![Mountains](docs/screenshots/mountains.jpg) | ![Cliffs and ramps](docs/screenshots/cliffs-ramps.jpg) |
| ![Start area](docs/screenshots/start-area.jpg) | ![River and forest](docs/screenshots/river-forest.jpg) |

## Building

Needs CMake 3.21+, a C++20 compiler and OpenGL 3.3. GLFW and stb are fetched at
configure time. On Linux, GLFW also needs the X11 or Wayland development
packages (`libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
libxkbcommon-dev libgl-dev` on Debian/Ubuntu; add `-DGLFW_BUILD_WAYLAND=OFF` if
you have no Wayland headers).

```sh
cmake -S . -B build
cmake --build build -j
./build/game/zillions --seed 7
ctest --test-dir build        # headless tests
```

`-DZL_BUILD_APP=OFF` builds only the headless parts (generator, tests, tools),
which need no windowing system or GPU.

## Running

```
zillions [--seed N] [--size N] [--width W --height H] [--no-vsync]
         [--screenshot FILE [--whole-map | --center X Y] [--zoom PIXELS]]
         [--overlay height|pass] [--grid] [--bench]
```

| Input | Action |
|---|---|
| WASD / arrows, right-drag | pan (Shift: faster) |
| mouse wheel | zoom about the cursor |
| R or Space | generate the next seed |
| F / H | frame the whole map / the start area |
| G | tile grid |
| V | cycle debug overlay: height levels, passability |
| F2 | save a screenshot |

`zl_mapgen` is the headless generator: `--out preview.png` writes a flat-colour
preview, `--bench` times generation.

`zillions --bench` times the renderer off-screen, then stress-tests the sprite
batch with 10,000 to 250,000 sprites that all move every frame.

The overlays draw gameplay data per tile, exactly as the simulation sees it.
Height shows level 0 in blue, 1 in yellow, 2 in red, ramps in white and a black
line on every border a cliff blocks:

![Height overlay](docs/screenshots/overlay-height.jpg)

## Layout

```
engine/core      types, RNG and hashing, job system, clock, images
engine/math      vectors, simplex / fBm / ridged noise
engine/platform  window, OpenGL context, input (GLFW)
engine/gfx       shaders, textures, render targets, 2D camera, sprite batch
game/world       tile map, map generator, CPU preview        (headless)
game/render      terrain shader, relief/erosion, procedural art
game/app         the viewer executable
tools/           zl_mapgen
tests/           headless test suite
```

`zl_core` and `zl_world` have no dependency on a window or GPU, so the
simulation can run in tests, tools and (later) a server.

## Engine notes

- **Two draw calls per frame.** The ground is one full-screen shader pass: each
  pixel looks up its four nearest tiles in a data texture and blends their
  materials, so cost depends on screen resolution, not map size. Everything
  standing on the ground is one instanced draw: 28 bytes per sprite written
  straight into mapped GPU memory, quad corners derived from `gl_VertexID`.
- **Job system.** A persistent thread pool with a blocking `parallel_for`;
  chunks are claimed with a single atomic add.
- **Determinism.** All randomness comes from seeded hashes and PCG32; noise is
  stateless. A seed produces the same map on any number of threads (tested).
- **Fixed timestep** scaffolding is in place for the simulation.
- **No art files yet.** Ground textures and sprites are drawn in code at
  start-up, as placeholders that can be swapped for real art later.

## Map generation

`generate_map()` takes a seed and a few coverage parameters and runs these steps:

1. **Noise fields** (parallel): domain-warped fBm for rolling land and lake
   basins, plus ridged noise gated by a low-frequency mask so mountains gather
   into ranges. Sea and mountain levels are taken as percentiles, so requested
   coverage holds for every seed.
2. **Start area**: elevation near the centre is squeezed into the plains band.
3. **Lakes**: everything below sea level, minus ponds and islets.
4. **Rivers**: a priority flood from every lake and map edge gives each tile a
   downhill neighbour; summing upstream area gives flow; tiles with enough flow
   become river. Rivers therefore rise in high ground, merge, never run uphill
   and end in a lake or leave the map.
5. **Mountains**: consolidated with a majority filter; specks are dropped.
6. **Height levels**: smoothed elevation is cut into three flat levels at
   percentiles. Ledges too narrow to use, including the thin skirts that
   would otherwise ring every mountain, are lowered, and the start area is
   kept on a single level.
7. **Terrain**: beaches along lakes, forest in groves biased to wet ground,
   dry patches, scree at the foot of mountains.
8. **Fords**: shallow crossings on narrow river reaches.
9. **Ore**: stone, iron and gold where mountains meet open ground, richer ores
   further out; stone and iron are guaranteed near the start.
10. **Ramps**: slopes three tiles wide cut into straight stretches of cliff,
    spaced well apart.
11. **Connectivity**: any sizeable region the colony cannot reach is joined by
    the cheapest cut (a ford, a pass, a gap in the trees or a ramp).
12. **Derived data**: walkable/buildable flags, water depth, statistics.

### Height

Every tile has a height level (`TileMap::level`, 0 to 2). Two neighbouring
tiles on different levels are separated by a cliff, which blocks movement
unless one of them is a ramp. `TileMap::can_step(from, to)` is the single rule
for this, and the generator's own reachability analysis uses it. Ramps are
walkable but never buildable, so the routes between levels stay open.
Mountains are separate from levels: they are impassable everywhere.

Height does not affect combat or vision yet; those arrive with units and towers.

For rendering, levels become terraces with rock-faced cliffs and sloping
ramps, and mountains are carved by an erosion model
(`game/render/relief.cpp`): drainage is routed from every mountain texel to the
foot of its range, and heights are rebuilt upwards with a slope that shrinks as
drainage area grows. Channels become valleys, divides become ridges, and the
ridges branch into spurs.
