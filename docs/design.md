# Zillions: game design

## Core idea

Zillions is a colony-survival strategy game. You build and fortify a colony on
a procedurally generated map, gather wood, stone and ore to grow it, and hold
out against ever-larger zombie hordes that pour in from the map edges.

**The terrain is the defence.** Cliffs, rivers and mountains funnel the horde
through ramps, fords and passes. The game is about choosing which chokepoints
to wall and hold before the next wave arrives. One breach can cascade and lose
the colony.

## Decided so far

- **Inspiration:** *They Are Billions*.
- **View:** 2D isometric on a tile grid, with terrain height and 90° camera rotation (Q / E).
- **Scale:** hordes in the thousands, which is why the game sits on its own
  C++ engine built for throughput.
- **Maps:** generated from a seed, with the colony starting at the centre on
  open, level ground with stone and iron in reach.
- **Grid:** terrain is sized in world units and the tile grid is finer than
  that: 2 tiles per unit by default, adjustable to 1 or 4. A fine grid is what
  lets a chokepoint hold a crowd and a small area hold many buildings.
- **Height:** open land sits on three levels. Cliffs block movement; ramps are
  the only way between levels and can never be built on, so the routes stay
  open for both sides.
- **Two scales of terrain:** lakes, ranges, cliffs and rivers are sized in
  world units and shape the strategy. Copses, lone trees, boulder outcrops and
  ponds are sized in tiles and shape the base: they are the small obstacles a
  colony has to build around, and they keep any screenful of map busy.
- **Resources on the map:** forests, and stone, iron and gold deposits where
  mountains meet open ground. Richer ores lie further from the colony.
- **The player is the plague doctor.** A skinned glTF model with Idle, Walking,
  Running, an overhead hammer swing and a sword slash; the 3D axe and pickaxe
  hang from its right hand. The procedural human remains as the missing-model
  fallback. See [the plague doctor notes](plague-doctor.md).

## Status

| Part | State |
|---|---|
| Engine core and renderer | built |
| Map generator with height levels | built |
| Player character: plague doctor GLB, skeletal animation and 3D tools | built; procedural human remains the missing-model fallback |
| Gathering, inventory, stone tools, crafting and camp storage | first playable loop built; see [survival notes](survival.md) |
| Colony: buildings and economy | not started |
| Horde: units, pathfinding, waves | not started |
| Combat, towers, high-ground effects | not started |

## Open questions

- How the horde escalates: timed waves, a final assault, or both.
- How a breach spreads through the colony.
- What high ground gives a defender: range, vision, damage.
- How many ramps a map should have. The generator currently places roughly
  40 to 50 on the default 512×512 map, which is a guess until there is a horde to test
  it against.
- Win condition: survive a fixed number of days, or something else.
