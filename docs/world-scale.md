# A small survivor in an enormous wilderness

The default world is now **4096 × 4096 tiles**, up from 512 × 512: eight times
the width and **64 times the area**. One tile is 1.25 metres at the player's
existing scale, so the world spans **5.12 km per side, or 26.2 km²**. Walking
and running retain their physical speeds. Crossing the map at an uninterrupted
5.2 m/s sprint would take about 16 minutes; terrain and obstacles make real
journeys longer.

The player stays 1.70 m tall. Broadleaf trees stand 8 m tall and pines
11 m tall before seeded variation (6.4–9.2 m and 8.8–12.65 m respectively).
Their heights are roughly 60% lower than the initial giant-tree pass. Crowns
and trunk footprints are narrower too, so paths stay accessible and trees fit
the normal play view. A single physical catalogue in
`game/world/scenery.h` defines mesh dimensions and collision footprints for
all scenery. Trees collide at their trunks, leaving space under the branches.
Rock footprints share their mesh's seeded rotation. Interaction selection and
reach account for larger trunks and outcrops.

Follow mode starts at 32 px/tile and management mode at 12 px/tile. Existing
zoom controls, the far-view player marker and foreground reveal still work.
Mountain relief is taller, so ranges remain monumental beside the mature trees.

## Generated scenery

Every existing world prop family now has an embedded-texture GLB under
`assets/models/scenery`:

- broadleaf, conifer and bush;
- stone, iron, gold and boulder;
- grass tuft, flower, pebble and reed;
- fallen branch, stump, rubble and storage chest.

These are real static meshes, rendered from all four camera orientations. The
existing player, axe and pickaxe meshes remain in use. Terrain, cliffs, water
and mountains already have generated geometry; UI icons remain images.

Models were produced through Meshy's REST API with `meshy-7.1`, followed by
texturing. Their combined geometry is 22,235 triangles across the 15 assets;
each is instanced wherever needed. A material review added object-specific
bark, grey stone, rusty iron and ochre gold descriptions. Production used
600 credits, including that second texture pass. The manifest records exact
prompts, task IDs, triangle counts, sizes and SHA-256 hashes. It contains no
API keys or expiring download links.

`tools/art/generate_scenery.py` can regenerate missing assets using the
`MESHYAI` environment variable. It preserves task IDs in ignored `work/`,
resumes polling, downloads results immediately and reports failures with a
nonzero exit status. Existing model files are skipped. Generating new files
spends Meshy credits.

## Biome density

World generation retains a continuous ecology field, sampled every four tiles
and interpolated between samples. Its woodland, meadow, scrub and rock values
are independent concentrations: they can overlap, and they fade across an area
instead of changing abruptly at a terrain label. The field costs about 16 MB
on the full-size world. Domain-warped regional woodland noise contains smaller
groves and clearings; feature wavelengths stay fixed as the map gets larger.

The default woodland target is 34% of eligible land, with more small features
than before. Dense forest cores support about 2.4 times the former maximum tree
density. Jittered candidates are 2.5 tiles apart before variation, with a minimum
two-tile separation. Tree probability falls smoothly through the fringe and
into open grass. Forest floors now contain grasses, shrubs and fallen branches.
Meadows concentrate flowers and long grass, scrub districts concentrate bushes,
rock districts concentrate pebbles and outcrops, and reeds fill wider shore bands.
The same woodland field blends meadow ground into forest litter. The later
[terrain material pass](terrain-materials.md) thins loose debris and ground
cover substantially, while retaining these dense tree cores and biome gradients.

Small copses and outcrops contribute feathered density patches to their
surroundings. Stone, iron and gold extend into foothills; the deposit budget is
40% larger, with closer spacing. Starter stone and iron remain guaranteed.
Trees avoid the start clearing, ramps, water, mountain walls and ore footprints;
nonblocking undergrowth adds density without closing walking gaps. Generation
retains the existing connectivity repair and deterministic seed behaviour.

## Keeping the world practical

- Scenery instances live in 32 × 32 tile spatial buckets. Only visible
  buckets contribute to mesh submission; small subpixel objects are culled.
- Meshes share GPU geometry and textures. Their actual geometry casts sun
  shadows and writes scene depth, including the foreground player reveal.
- At map overview zoom, subpixel scenery and its mesh shadows are omitted;
  terrain materials carry forests and resources.
- Smooth biome weights modulate a spaced tree candidate grid, giving dense
  cores, sparse fringes and clearings without overlapping trunk collisions.
- Relief is capped at 4096 texels per side. Erosion, smoothing, normals and
  mesh tessellation use its effective resolution, avoiding an 8192² scratch grid.
- Art reload rebuilds the cached object representation, so changing between
  meshes and sprite fallbacks cannot lose scenery or resurrect harvested props.
- Large interactive maps generate on a background thread while the window
  displays an animated loading indicator and continues processing events.

A seed-7 default-size render with the density pass completed successfully on
the managed software OpenGL environment, with a measured peak of about 2.1 GB. Startup
and frame times depend on hardware. Smaller development maps remain available
with `--size 512` or `--size 128 --resolution 1`.

## Verification

```sh
cmake --build build -j 5
ctest --test-dir build --output-on-failure
./build/tests/zl_biome_tests work/review
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_scenery_render_tests work/review
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_render_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_survival_render_tests
```

Headless checks load all 15 generated files, check mesh integrity and texture
decoding, and verify human/tree proportions and trunk clearance. Biome checks
measure density gradients, species concentrations, forest understory, tree
spacing, protected starts and ramps, smooth regional variation and treeless
settings. Ecology fields are included in the cross-thread determinism checks.
GPU checks
exercise all four views, real depth writes, harvesting removal, offscreen
canopy culling and all existing terrain, player and survival rendering paths.
