# AI art mock-ups

Generated with an image model to preview what real art could look like in the
game. These are **not** engine renders and not wired into the build; the game
still draws all art procedurally in code (`game/render/terrain_art.cpp`).

- `broadleaf-tree-sprite.png` — the generated sprite itself (RGBA, alpha-matted
  locally), trimmed to content with the anchor at the trunk base.
- `sprite-on-tile.png` — the sprite at 2 tiles wide (32 px per tile) on a
  grass patch, showing in-game scale and anchoring.
- `map-overview.png` — seed 7 map (`zl_mapgen --seed 7 --scale 4`) with the
  sprite scattered on forest tiles.
- `start-area-zoom.png` — 96x96 tile crop around the start area, sprite on
  forest tiles at 16 px per tile.
- `forest-zoom.png` — densest 96x96 tile forest window on seed 7, same scale.

The map previews come from the headless generator; the trees were composited
on forest tiles in a local script to approximate the sprite pass.
