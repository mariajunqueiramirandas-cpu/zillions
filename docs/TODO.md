# Zillions: to do

Open work after the painted art was loaded into the viewer. Grouped by area,
most useful first.

## Art

- [ ] Palette: forest, sand and shallows are outside their brief ranges (see
      the palette review in `docs/art/showcase.md`). Regenerate or retouch the
      source sheets, then re-export with `tools/art/export_assets.py`.
- [ ] Sprites: judge contact shadows and far-zoom readability in the viewer;
      retouch any sprite that is lost at 16 px per tile.
- [ ] Trees: the broadleaf variants 3 to 5 are thinner than the brief asks for
      in places; check they still read as a solid mass.
- [ ] Decide the final delivery scale. The art is authored at 32 px per tile;
      if files move to 2x, `kArtTilePixels` and every anchor must change too.

## Shader

- [ ] Tune `WATER_*`, `FOAM`, `SCRUB` and `HAZE` in `kTerrainFragment` after a
      look at a full map, not just the start area.
- [ ] Check the overall desaturation and haze on the far zoom levels, where the
      ground textures carry the picture alone.

## Engine and tools

- [ ] Hot-reload the art, or at least a key to reload it without restarting.
- [ ] Mipmapped sprite atlas bleeding: verify the 4 px padding holds at the
      smallest zoom with the painted (not generated) edges.
- [ ] Decide whether `tools/art/export_assets.py` should be run in CI, or only
      by hand when the sources change.

## Game (not started)

- [ ] Buildings, units, the horde and combat. Their art is out of scope until
      the design settles (see `docs/design.md`).
