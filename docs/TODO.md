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

## Game (not started)

- [ ] Buildings, units, the horde and combat. Their art is out of scope until
      the design settles (see `docs/design.md`).
