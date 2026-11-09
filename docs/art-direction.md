# Zillions: art direction

This is the brief for replacing the placeholder art with real artwork. It lists
every image the game can show today, says what each one has to look like, and
gives the exact sizes the renderer expects.

It is written to be read on its own. Where it helps to see the source, the
placeholders are drawn in `game/render/terrain_art.cpp` and used by
`game/render/map_renderer.cpp`.

## Scope

**In this batch: 47 images.**

| Group | Count | What |
|---|---|---|
| Ground textures | 7 | grass, forest floor, dirt, rocks, sand, mountain, shallows |
| Scenery sprites | 40 | 11 families: trees, bushes, ore deposits, boulders, ground cover |

**Not in this batch:**

- **The water texture and the noise texture.** They are data the shader reads
  channel by channel, not pictures. See [Water](#water-data-not-a-painting).
- **Anything the terrain shader draws by itself:** borders between terrain
  types, cliff faces, ramps, the shape and shading of mountains, water colour
  and movement, shoreline foam, shadows cast by high ground. None of these need
  an image, so there are no transition tiles or autotile sets to make.
- **Buildings, units, the horde, effects and UI.** The game design has not
  decided what these are yet (`docs/design.md` lists them as not started), so
  there is nothing to specify. Do not invent them. The style rules below will
  apply to them when they are designed.

## The look

A bright, clean, hand-painted landscape seen from above: saturated greens,
warm earth, clear blue water. Shapes are simple and cel-shaded, with a small
number of flat tones and a thin dark outline. Think of a well-kept board game
map more than a photograph.

The landscape stays calm and readable on purpose. The player reads the terrain
to choose where to defend, and later thousands of units will move across it, so
the ground must never compete with what stands on it.

Zillions takes its gameplay from *They Are Billions*, not its art. Do not
imitate the look of that game or any other.

### Rules every image follows

1. **Light comes from the upper left.** Lit faces are up and left, shaded faces
   are down and right, and cast shadows fall to the lower right. The shader
   uses the same light for the terrain (direction `(-0.50, -0.62, 0.60)`, with
   y pointing down the image), so a sprite lit from any other side will look
   pasted on.
2. **Cel shading in three or four tones.** Each material has a shadow tone, a
   base tone, a light tone and at most one highlight. Tones meet at a clean
   edge, broken up into clusters (leaf clumps, rock facets), not blended with a
   smooth gradient.
3. **A thin coloured outline.** About 1 px at authoring size, in a very dark
   version of the object's own colour (dark green for foliage, dark grey-brown
   for rock). Never pure black.
4. **Readable when small.** The camera zooms out until the whole map fits and
   in to 128 screen pixels per tile; the default is 32. Check every image at
   8, 16, 32 and 64 pixels per tile. Each sprite must be recognisable from its silhouette
   alone at 16.
5. **Function before decoration.** Things that block movement (trees, boulders,
   ore deposits) look solid and tall, with a clear shadow. Things that do not
   (bushes, tufts, flowers, pebbles, reeds) look low and light. The player
   should be able to tell the two apart without thinking.
6. **Paint at full brightness and neutral colour.** The engine multiplies each
   sprite by a per-instance tint that only darkens it (down to 84%) and shifts
   it slightly warm or cool, and it tints grass and forest ground by moisture.
   Variation between instances comes from there, so do not paint it in.

## How the renderer uses the art

These facts explain most of the constraints below.

- **Scale.** One tile is 32 × 32 px at authoring size
  (`kArtTilePixels`). Every size in this document is in pixels at that scale.
  The camera can magnify up to 4×, so it is worth painting masters at 2× or 4×
  and downsampling. If the final files are delivered at 2×, every size and
  anchor here doubles and `kArtTilePixels` must change to 64; say so in the
  handoff.
- **Ground.** One texture per terrain type, 256 × 256 px, covering 8 × 8 tiles
  and repeating forever. The shader picks the texture per tile, blends
  neighbouring terrains along a wandering border, then multiplies the result by
  lighting, shadow, a height tint and a large-scale light/dark variation.
- **Sprites.** All sprites are packed into one 1024 × 1024 atlas with
  mipmaps and drawn with premultiplied alpha. Each sprite has an **anchor**:
  the pixel that sits on the object's position on the ground. Sprites are drawn
  north to south, so an object overlaps whatever is behind it.
- **No rotation and no mirroring.** A sprite is only ever scaled (roughly 0.7×
  to 1.4×) and tinted. Variety has to come from the variants.
- **Far zoom.** Below 2.5 screen pixels per tile no sprites are drawn at all,
  and the ground textures alone carry the picture. A forest must still read as
  a forest there, which is why the forest floor is much darker than grass.

## Ground textures

### Shared requirements

- 256 × 256 px, opaque, sRGB. All seven must be exactly the same size because
  they are layers of one texture array.
- **Seamless on all four edges.** Tile it 3 × 3 and look for seams.
- **No visible repeat.** The texture repeats every 8 tiles, so nothing in it
  may be larger than about one tile (32 px) or stand out from its neighbours:
  no single big stone, no dark patch, no long crack. Keep detail even across
  the whole image.
- **Flat lighting.** No directional light, no gradients, no baked shadows of
  other objects. The shader adds all of that. The one exception is small
  pebbles, which may have a lit upper-left edge and a darker lower-right edge.
- **Stay inside the palette.** The shader multiplies these colours, so a
  texture that is much brighter, darker or more saturated than its palette
  shifts the whole map. Keep the average colour between the two values given.
- **Low contrast.** Roughly ±10% around the average. The ground is a backdrop.

### Grass

Open, buildable ground and the most common thing on screen.

- **Palette:** `#548636` to `#76A040`.
- **Content:** short mown-meadow grass. Soft mottling a few tiles across, and
  short vertical strokes that read as blades. No flowers, weeds or stones;
  sprites add those.
- **Watch for:** the shader shifts grass yellower on dry ground and deeper
  green on wet ground. Paint a neutral mid-green, not a yellow-green or a
  blue-green, or one of those shifts will go muddy.

### Forest floor

The ground under trees. Mostly hidden by canopies, visible in gaps, at the
edges of groves, and everywhere at far zoom.

- **Palette:** `#2C5228` to `#44642E`, with patches of leaf litter `#524E2C`.
- **Content:** dark, shaded undergrowth: moss, low ferns suggested by texture,
  scattered brown leaf litter. No trunks, roots or logs.
- **Watch for:** it must be clearly darker than grass so that groves read as
  dark masses when the tree sprites are not drawn.

### Dirt

Bare, dry earth: buildable ground interleaved with grass, and the apron around
boulder outcrops and mountain feet.

- **Palette:** `#806646` to `#A0845C`.
- **Content:** packed earth with fine grain and a light scatter of small
  pebbles (2 to 5 px across, about one every 15 px).
- **Watch for:** keep it warm and matte. It must not read as sand (paler,
  yellower) or as rock (greyer).

### Rocks

Gravel under a boulder outcrop. A boulder sprite always stands on each of these
tiles, so this is only the ground between and around the boulders.

- **Palette:** `#766C5C` to `#968C7A`.
- **Content:** broken stone and coarse gravel, denser and greyer than dirt,
  with pebbles of 3 to 6 px about every 10 px, roughly twice as many as on
  dirt.
- **Watch for:** no stone large enough to be mistaken for a boulder.

### Sand

Beaches along lake shores.

- **Palette:** `#CEBA84` to `#E4D4A0`.
- **Content:** smooth pale sand with very faint wind ripples running roughly
  left to right, and a rare tiny pebble. The quietest texture of the set.
- **Watch for:** ripples must stay faint (2 to 3% brightness). Strong lines
  make the 8-tile repeat obvious.

### Mountain

Rock grain only. The shader supplies everything else about a mountain: its
shape, light and shade, and its colour from scrubby green foothills to pale
summits. The same texture is reused at twice the density for cliff faces, where
it is brightened 2× and tinted warm.

- **Palette:** neutral grey centred on `#84827E`, close to 50% brightness.
- **Content:** fine stone grain, hairline cracks and thin near-horizontal
  strata lines.
- **Watch for:** no hue, and nothing brighter than about 55% grey, because
  cliffs double the value and anything brighter clips to white. It has to look
  right both at one repeat per 8 tiles (mountains) and one per 4 tiles
  (cliffs).

### Shallows

The bed of a river ford, seen through clear water. The shader blends this 42%
towards teal and lays moving light patterns over it.

- **Palette:** `#849680` to `#A8B08C`.
- **Content:** a river bed of rounded pebbles (3 to 6 px) in grey-green silt.
  Paint the dry bed only: no water, ripples or highlights.
- **Watch for:** give the pebbles a little more contrast than looks right on
  its own, because the water layer flattens it. A ford has to read as "you can
  cross here" next to deep water.

### Water (data, not a painting)

Leave this one generated in code. It is listed so nobody paints over it. Its
three channels are separate greyscale patterns that the shader scrolls against
each other: red and green are two different webs of thin bright caustic lines
on black, and blue is a soft ripple field centred on mid-grey. Water colour,
depth and foam are computed. The same applies to the ninth "noise" layer.

## Sprites

### Shared requirements

- One PNG per sprite, RGBA, sRGB, **straight (not premultiplied) alpha**, at
  exactly the canvas size listed. The atlas packer premultiplies.
- Transparent background with at least 1 px of fully transparent margin on
  every side. No stray semi-transparent pixels away from the object; they show
  up as halos once the atlas is mipmapped.
- The object's base (the bottom of a trunk, the middle of a rock's footprint)
  sits on the **anchor** pixel listed, measured from the top-left corner.
- **Contact shadow painted into the sprite:** a soft ellipse of `#050D0A`
  at 30 to 40% opacity in the middle, fading to nothing at its edge, lying on
  the ground under the object and pushed slightly to the lower right. It is
  wide and flat, about 2.5 times wider than tall. Tufts, flowers and reeds
  have no shadow.
- Variants of a family share the canvas size, the anchor, the palette and the
  overall mass. They differ in silhouette. Because sprites are never mirrored,
  make some variants lean or bulge left and others right.

### The eleven families

| Family | Variants | Canvas (px) | Anchor (px) | Size in tiles | Blocks movement |
|---|---|---|---|---|---|
| `broadleaf` | 6 | 64 × 76 | 32, 66 | 2.0 × 2.4 | yes |
| `conifer` | 4 | 44 × 80 | 22, 71 | 1.4 × 2.5 | yes |
| `bush` | 3 | 34 × 26 | 17, 20 | 1.1 × 0.8 | no |
| `stone` | 3 | 46 × 40 | 23, 27 | 1.4 × 1.25 | yes |
| `iron` | 3 | 46 × 40 | 23, 27 | 1.4 × 1.25 | yes |
| `gold` | 3 | 46 × 40 | 23, 27 | 1.4 × 1.25 | yes |
| `boulder` | 4 | 46 × 44 | 23, 31 | 1.4 × 1.4 | yes |
| `tuft` | 4 | 20 × 16 | 10, 14 | 0.6 × 0.5 | no |
| `flower` | 4 | 20 × 16 | 10, 14 | 0.6 × 0.5 | no |
| `reed` | 3 | 20 × 30 | 10, 28 | 0.6 × 0.9 | no |
| `pebble` | 3 | 22 × 14 | 11, 9 | 0.7 × 0.4 | no |

### Broadleaf tree

The tree of lowland and wet forest. Each forest tile holds one or two, and
because a crown is two tiles wide on a one-tile footprint, neighbouring crowns
overlap into a continuous canopy. Lone trees also stand in open grass.

- **Shape:** a round, full crown made of five to seven overlapping leaf clumps,
  on a short brown trunk that shows for about a fifth of the height. The crown
  is slightly wider than tall.
- **Shading:** four tones, with the lightest clumps at the upper left and the
  darkest under the lower right. Tone edges follow leaf clusters.
- **Palettes:** three, two variants each, so a forest is not one flat green.

  | | Shadow | Base | Light | Highlight |
  |---|---|---|---|---|
  | A, mid green | `#1E4C28` | `#326E30` | `#549038` | `#7EB04A` |
  | B, yellow green | `#245222` | `#40762A` | `#689632` | `#96B646` |
  | C, blue green | `#18462E` | `#286438` | `#428442` | `#68A454` |

  Outline `#12301E`. Trunk `#684C32` on the lit side, `#483424` in shade.
- **Watch for:** the canopy is the main thing. It has to look good as a mass of
  thirty overlapping crowns, so keep each crown's outline clean and its
  darkest tone at the bottom edge, where the tree in front will overlap it.

### Conifer

The tree of higher, drier ground. Forests shift from broadleaf to conifer with
altitude, and the two mix along the boundary.

- **Shape:** a narrow spire of four stacked tiers of drooping boughs with a
  ragged, saw-toothed lower edge, on a short trunk. Clearly taller and
  narrower than the broadleaf, with a pointed top.
- **Shading:** three tones. Light on the left of each tier, and the underside
  of each tier in shadow so the tiers separate.
- **Palette:** shadow `#163C2E`, base `#24583A`, light `#3E7848`, outline
  `#0E2822`. Darker and bluer than any broadleaf, so the two kinds of forest
  can be told apart at a glance.
- **Variants:** vary the lean of the trunk by a pixel or two and the raggedness
  of the tiers.

### Bush

A low shrub scattered on open grass, in thickets. Units walk through it and
buildings go over it.

- **Shape:** a rounded mound of three or four leaf clumps, wider than tall, no
  visible trunk, sitting directly on the ground.
- **Palette:** shadow `#265426`, base `#3E762E`, light `#629638`, highlight
  `#8CB44C`, outline `#16341E`.
- **Watch for:** it must not look like a small tree. Keep it to about half a
  tile tall, with a lighter shadow than a tree's.

### Stone, iron and gold deposits

The three resources on the map. A deposit is a cluster of tiles with one of
these sprites on each, so the sprites sit shoulder to shoulder and have to
work as a group. Telling the three apart instantly, at any zoom, is the most
important readability requirement in this brief.

All three share one form: a pile of three or four angular, faceted rocks with
flat tops, seen slightly from above, the nearer rocks overlapping the further
ones. They differ in colour.

- **Stone:** pale, cool grey rock, `#A8A8B0`, outline `#36363C`, with a few
  lighter flecks. The cleanest and brightest of the three.
- **Iron:** dark red-brown rock, `#78564A`, outline `#34221E`, streaked and
  spotted with rust orange `#CE682C` over about a third of its surface.
- **Gold:** plain grey-brown host rock, `#8A847C`, outline `#36363C`, with
  veins and nuggets of bright yellow `#F4C436` over about a fifth of its
  surface and a few near-white glints `#FFF096`.
- **Watch for:** stone must not be confused with the `boulder` family, which
  is scenery. Deposits are cleaner, cooler and more angular. Gold must stay
  visible when the sprite is 12 px wide, so use a few bold patches of yellow,
  not fine speckle.

### Boulder

One or two large weathered rocks: the obstacle on an outcrop tile. These are
the small features a colony has to build around.

- **Shape:** one big rounded-angular boulder, in some variants with a smaller
  one in front of it. Heavier and rounder than a deposit.
- **Palette:** warm grey-brown `#8E867A`, outline `#34302E`. A little moss or
  lichen is welcome, kept subtle.
- **Variants:** two with a single boulder, two with a pair.

### Tuft

A fan of grass blades. Decoration on grass, and sparsely on dirt, where the
engine tints it warmer.

- **Shape:** five to seven thin blades spreading from one root point, the
  outer ones shorter and leaning outward.
- **Palette:** blades between `#3E702C` and `#84B246`. No outline and no
  shadow; at this size they would swallow the sprite.

### Flower

The tuft with flower heads on about half the blades. Flowers grow in drifts
that become meadows, so one sprite is seen in groups of dozens.

- **Shape:** as the tuft, with small round heads about 3 px across.
- **Palette:** each variant uses one petal colour: cream `#F6F0DE`, yellow
  `#F4D048` or violet `#C484D6`. Four variants, so one colour appears twice
  with a different arrangement.
- **Watch for:** the petals are the brightest, most saturated pixels on the
  ground. Keep them small.

### Reed

Tall waterside grass. Appears only on grass tiles that touch water, so it
draws the shoreline.

- **Shape:** five to seven tall, nearly upright blades, more than twice the
  height of a tuft, with slim dark seed heads on a few of them.
- **Palette:** blades between `#466832` and `#809C4E`, duller and more olive
  than tufts. Seed heads `#704C2C`.

### Pebble

Two or three small stones. Scattered on dirt, rarely on sand, and in fords,
where the engine tints them cooler and they read as stones standing out of the
water.

- **Shape:** small rounded stones, flat and low, with a faint shadow.
- **Palette:** `#96928C`, outline `#46423E`.
- **Watch for:** clearly smaller and flatter than a boulder. Nobody should
  think a pebble blocks the way.

## Delivery

The engine cannot load image files yet: today the art is generated in code at
start-up and `engine/core/image.h` can only write PNGs. Wiring a loader in is a
separate task from producing the artwork. To make that task simple, deliver
files under this layout:

```
assets/ground/grass.png
assets/ground/forest.png
assets/ground/dirt.png
assets/ground/rocks.png
assets/ground/sand.png
assets/ground/mountain.png
assets/ground/shallows.png
assets/sprites/<family>_<n>.png     e.g. broadleaf_0.png ... broadleaf_5.png
```

Family names are the ones in the table above; variants are numbered from 0.
For `broadleaf`, variants 0 and 3 use palette A, 1 and 4 palette B, and 2 and
5 palette C.

### Checklist before handing back

- [ ] 7 ground textures, each 256 × 256, opaque, and seamless when tiled 3 × 3.
- [ ] No ground texture shows a repeat when tiled 6 × 6 and viewed at 25%.
- [ ] Each ground texture's average colour lies inside its palette range.
- [ ] `mountain.png` is neutral grey with no pixel above about 55% brightness.
- [ ] 40 sprites with the exact canvas sizes and file names listed.
- [ ] Every sprite's base sits on its anchor pixel.
- [ ] Every sprite is lit from the upper left, with its shadow to the lower
      right.
- [ ] Stone, iron and gold can be told apart at 16 px per tile.
- [ ] Blocking objects (trees, deposits, boulders) look solid; walkable ground
      cover does not.
- [ ] A contact sheet of all sprites on a grass background, at 1× and 2×, is
      included for review.
