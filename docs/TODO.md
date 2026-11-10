# Zillions: to do

Painted-art integration is complete. The checks below record the integration
review; remaining unchecked items are future artwork polish or game features.

## Art

- [ ] Palette: forest, sand and shallows are outside their brief ranges (see
      the palette review in `docs/art/showcase.md`). Regenerate or retouch the
      source sheets, then re-export with `tools/art/export_assets.py`.
- [x] Sprites: reviewed contact shadows and readability in the viewer at 8,
      16, 32 and 64 px per tile. Small ground cover stays quiet and walkable;
      trees and deposits retain distinct silhouettes.
- [x] Trees: checked variants 3 to 5 in overlapping groves. The thin crowns
      remain recognisable as trees; fuller crowns can be an artwork revision.
- [x] Keep this delivery at 32 authoring px per tile and the existing anchors.

## Shader

- [x] Reviewed `WATER_*`, `FOAM`, `SCRUB` and `HAZE` on full maps (seeds 3 and 7)
      and the start area. Fords remain paler than water and cliffs read clearly.
- [x] Checked desaturation and haze on a full 1024 × 1024 map, below the sprite
      cutoff, where forest ground alone must distinguish groves from grass.

## Engine and tools

- [x] F5 reloads art, with fallback and recovery for either group, preserving
      the map and view. Verified in the live viewer with removed/restored files.
- [x] Replaced 4 px atlas padding with aligned 16 px padding and a level-4 mip
      limit. Tested every sprite's texels and guard space; reviewed rendering
      at 2.5 px per tile, just before scenery is culled.
- [x] Keep export manual when source paintings change. Normal builds consume
      committed PNGs; headless tests check decoding, packing and fallback.

## Character (complete)

The playable human, movement and rendering review is complete. Reproduction
commands, pose and scene images, test coverage and benchmark measurements are
in `docs/character.md`.

### Look

- [x] Face: smaller, lighter eyes and brows; facial details bypass contact
      shadows. Reviewed the 256 px/tile close-ups.
- [x] Head: raised face directions nine degrees to correct the downward read.
- [x] Hood cape: flatter, cut shoulder panels with dark hem bands; reviewed
      front, profile, back and raised-hood views.
- [x] Boots: shorter ball/toe reach, seven-degree standing turnout and a
      smaller heel offset/rear cap that keeps the heel under the ankle shaft.
- [x] Skirt: all three panels share a hem height, with vertical hem bands.
- [x] Reviewed walk, jog and sprint cycles, back views, all five hair styles,
      beard, hood, blinking and seeded looks. The sheet now guarantees these
      variants; shaved hair uses smooth stubble shading.

### In the scene

- [x] Trees: reviewed a dense grove against the map-only reference. Lowered
      foreground opacity to 24% and fade distance to 0.35 tiles so overlapping
      crowns keep the character readable; low ground cover stays solid.
- [x] Wading: reviewed ford waterline, sink and ripples. Spawn and screenshot
      poses take water depth from the tile; the shader clips the visible
      surface, and tree-reveal bounds include the sink offset.
- [x] Far-zoom marker: reviewed at zoom 6; the existing 13 px minimum radius
      and 0.62-tile radius remain legible.
- [x] Live play: exercised jogging, sprinting, strolling, turning, follow/free/
      return camera, look/hood/reset, screenshots and exit in the live window.
      Headless checks cover wall sliding and cliff/water blocking.
- [x] Shader cost: `--bench` includes the figure at 6, 64 and 256 px/tile.
      Conservative part bounds skip unnecessary cone/shadow work. The isolated
      256 px/tile software-renderer case fell from 80.0 to 53.7 ms.

### Tests (headless, in `tests/test_main.cpp`)

- [x] Skeleton: limb lengths, sole clearance, wrapped phase, part capacity,
      finite values and screen bounds across looks, speeds and cycle phases.
- [x] Gait: planted heel, ball and toe retain their ground positions through
      the flat stance interval; phase 0 matches phase 1.
- [x] Player: obstacles, cliffs, ramps, wall sliding, ford slowdown, exact
      input replays, spawn relocation, map edges and wrapped interpolation.
- [x] Looks: deterministic per seed and varied across seeds.

### Docs and housekeeping

- [x] README: controls/options, character section and pose sheet, actor layout
      and the four-call scene rendering path.
- [x] `docs/design.md`: character added to the status table.
- [x] `docs/character.md`: rig, gait, shader, images and validation evidence.
- [x] Update the description of PR #6 with the completed feature and checks.

## Game (not started)

- [ ] Buildings, units, the horde and combat. Their art is out of scope until
      the design settles (see `docs/design.md`).
