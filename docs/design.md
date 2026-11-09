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
- **View:** 2D, top-down, on a tile grid.
- **Scale:** hordes in the thousands, which is why the game sits on its own
  C++ engine built for throughput.
- **Maps:** generated from a seed, with the colony starting at the centre on
  open, level ground with stone and iron in reach.
- **Height:** open land sits on three levels. Cliffs block movement; ramps are
  the only way between levels and can never be built on, so the routes stay
  open for both sides.
- **Resources on the map:** forests, and stone, iron and gold deposits where
  mountains meet open ground. Richer ores lie further from the colony.

## Status

| Part | State |
|---|---|
| Engine core and renderer | built |
| Map generator with height levels | built |
| Colony: buildings and economy | not started |
| Horde: units, pathfinding, waves | not started |
| Combat, towers, high-ground effects | not started |

## Open questions

- How the horde escalates: timed waves, a final assault, or both.
- How a breach spreads through the colony.
- What high ground gives a defender: range, vision, damage.
- How many ramps a map should have. The generator currently places roughly
  55 to 65 on a 256×256 map, which is a guess until there is a horde to test
  it against.
- Win condition: survive a fixed number of days, or something else.
