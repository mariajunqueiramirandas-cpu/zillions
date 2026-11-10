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

## Character (in progress)

The player's character is in (`game/actor`, `game/render/figure_renderer.*`)
but unpolished. Each item says how it will probably be fixed; none is verified.

### Look

- [ ] Face reads as a dark smudge. Shrink the eyes and brows (they are sized
      for a larger head), lighten the eye colour, and exempt the `kFace`
      group from the self-shadow pass in the figure shader so the fringe and
      brow stop darkening it.
- [ ] Head looks tilted down. Features sit too low because forward offsets
      are drawn at `kFigureDepthScale`; raise the face directions in
      `skin()` by about 8 to 10 degrees, or lower the depth scale slightly.
- [ ] Hood cape looks like puffy blobs. Replace the round shoulder cones with
      flatter, flat-capped ones (lower `roundness`, `FlatCaps` hem) and add a
      darker hem band, as the tunic skirt has.
- [ ] Boots too long and splayed. Shorten `kBallAhead` and `kToeLength`, and
      cut the standing turnout from 13 to about 7 degrees in `pose()`.
- [ ] Skirt hem is stepped where panels meet. Give all three panels the same
      hem height (project the hem points to one plane), or replace the hem
      bands with one band per panel at equal `z`.
- [ ] Review close-ups not yet looked at: walk, run and sprint cycles, back
      view, each hair style, beard, hood up, random looks. Render with
      `--character-sheet F --zoom 256` and crop; fix what shows.

### In the scene

- [ ] Walking behind trees and the see-through fade. Screenshot with
      `--player X Y` just north of a grove; tune `kSeeThrough*` in
      `map_renderer.cpp`.
- [ ] Wading. Screenshot on a ford tile with a non-zero `wade`; check the
      waterline cut, the sink offset and the ripples in the figure shader.
- [ ] Far-zoom marker ring. Screenshot at `--zoom 6`; tune `kMarkerRadius`
      and `kMarkerPixels`.
- [ ] Live play. Run the window and check controls, camera follow, turning
      and sliding along cliffs and water; tune the rates in `Player::step`.
- [ ] Shader cost. Add a figure case to `--bench`; if slow, skip parts early
      with a bounding-circle test, or store bounds in a fifth texel.

### Tests (headless, in `tests/test_main.cpp`)

- [ ] Skeleton: thigh and shin keep their lengths, no sole point below the
      ground, pose at phase 0 equals phase 1, part count within
      `kMaxFigureParts`, all values finite.
- [ ] Gait: a planted foot does not slide (foot position plus distance
      travelled is constant through the flat part of stance).
- [ ] Player: cannot enter water, forest or mountain, cannot cross a cliff
      except by a ramp, slides along a wall, slows in a ford, and two runs
      with the same inputs match exactly.
- [ ] Looks: `random_look` is deterministic per seed and varies across seeds.

### Docs and housekeeping

- [ ] README: new controls and options, a character section with a pose
      sheet image, the `game/actor` entry in the layout, draw-call count.
- [ ] `docs/design.md`: add the character to the status table.
- [ ] New `docs/character.md`: the rig, the gait and the figure shader.
- [ ] Update the description of PR #6 when the above lands.

## Game (not started)

- [ ] Buildings, units, the horde and combat. Their art is out of scope until
      the design settles (see `docs/design.md`).
