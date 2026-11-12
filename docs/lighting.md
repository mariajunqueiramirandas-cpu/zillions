# Shared world lighting

Terrain, scenery and the player now use the same lighting and display functions
in `engine/gfx/lighting.h`. Base-colour textures are decoded from sRGB before
lighting. All light accumulation happens in linear space, then a single shared
highlight curve and sRGB conversion produce the displayed colour. Packed
terrain fields, material maps and UI colours remain data and are not gamma decoded.

The lighting uses a fixed world-space warm sun, cool hemispherical sky fill and
subtle ground bounce. Sky illumination keeps shaded surfaces readable. Direct
sunlight is attenuated by the shared shadow map. Camera rotation changes the
view direction used for reflections; it does not move a fill light around the
player. The previous camera-facing fill, fake rim, blanket brown haze and
`pow(albedo, 0.62)` brightness lift have been removed.

The shared BRDF combines diffuse reflection with a roughness-controlled GGX
specular lobe. The loader now reads glTF `metallicFactor`, `roughnessFactor`
and packed metallic-roughness textures (green = roughness, blue = metallic).
The player and tool scans include these maps; ignoring them previously gave
cloth, wood and metal the same invented highlight. Generated scenery also
uses its authored material factors. Base-colour factors and instance tints
are applied in linear space.

The display curve scales all three colour channels together and smoothly
compresses bright values below white. It preserves hue and prevents bright
surfaces from clipping into chalky flat patches. The fixed exposure and sun/sky
balance live in one place rather than separate constants in each renderer.

Sun shadows use a deterministic twelve-tap disk filter. Nearby blocker depth
keeps contact edges firm and lets tall-tree shadows soften over distance.
Painted fallback art retains its authored face shading; external shadows are
applied in linear space with correct premultiplied alpha.

All active scenery casts shadows, including grass, bushes, flowers and reeds.
The 4096² depth map fits the elevations of visible receiver chunks rather than
the tallest mountain anywhere in the world. Offscreen casters still use the
global light-depth range. This preserves the shadow detail of small plants on
large maps. Casters smaller than 1.5 shadow texels are omitted at distant zooms.

Gameplay, F2 captures, command-line screenshots and demo recordings render into
a native-resolution 4× MSAA target, clamped to the driver's supported sample
count. Colour and depth resolve into a single-sample target before readback;
the resolved colour is copied to the window without a spatial blur. The HUD is
drawn at the same native resolution. Scenery uses alpha-to-coverage for cutout
edges, placement previews and foreground reveal instead of screen-door pixel
discard. UVs use centroid interpolation and tiny props fade with projected
size instead of abruptly disappearing at a two-pixel threshold. Material
textures retain trilinear mipmaps.

Ground material variation is deliberately subtle. Terrain no longer applies
an extra directional-light multiplier before the shared lighting pass. Water
uses a smoother material response, with quieter caustic highlights. Grid,
passability and height overlays are applied after lighting so their debug
colours remain readable.

## Checks

`zl_lighting_tests` runs the actual GLSL on the GPU and checks readable shadow
detail, neutral surfaces, preserved pigment, roughness-controlled highlights,
monotonic HDR rolloff and dark-material contrast. The glTF tests check the real
player/tool material maps, while the UV fixture uses an explicit matte material
to isolate texture orientation from metallic reflections.

```sh
cmake --build build -j 5
ctest --test-dir build --output-on-failure
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_lighting_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_foliage_render_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_model_render_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_scenery_render_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_render_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_survival_render_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_figure_render_tests
```

Visual review includes matched seed-7 captures at normal play zoom, quarter-turn
camera rotation, the full 4096² world, a contact sheet of all fifteen scenery
families and the player's axe/pickaxe poses across all four camera angles.
The foliage checks measure ground-only shadows for four plant families at all
camera rotations, shadow removal after harvesting, fractional MSAA coverage,
subpixel camera motion, resolve/presentation state and target resizing.
