# Zillions: art direction

This is the brief for replacing the placeholder art with real artwork. It lists
every image the game can show today, says what each one has to look like, and
gives the exact sizes the renderer expects.

It is written to be read on its own. Where it helps to see the source, the
art is loaded in `game/render/terrain_art.cpp` and used by
`game/render/map_renderer.cpp`. The same file draws a placeholder in code for
every image, used when the files are missing. The placeholders are bright and
clean; they show the sizes and layout, **not** the look. The look is defined
here.

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
  an image, so there are no transition tiles or autotile sets to make. Their
  colours are set in code to fit this direction; see
  [Colours the shader owns](#colours-the-shader-owns).
- **Buildings, units, the horde, effects and UI.** The game design has not
  decided what these are yet (`docs/design.md` lists them as not started), so
  there is nothing to specify. Do not invent them. The style rules below will
  apply to them when they are designed.

## The look: the plague years, as painters show them

The reference is how artists have portrayed the time of the bubonic plague:
Bruegel's *The Triumph of Death*, the dark plague paintings of the Renaissance
and Baroque, and the grim medieval scenes of later history painting and modern
illustration.

Across all of them the picture is the same. A barren, scorched land lies under
a leaden, smoke-stained sky. There is mud, dead wood and still water. Shadows
are deep and brown-black, the light is dull and sickly, and there is almost no
colour except earth, bone and an occasional red.

The map is that land seen from above. Grass is sallow and withered, trees are
gaunt and half bare, water is murky, and the light is always overcast. Nothing
is lush and nothing is clean.

Every image should look like **a small piece of a dark oil painting**: opaque
paint and visible brushwork, a few sombre values, shadows that sink into warm
near-black, and edges that are crisp where the light falls and lost where it
does not.

Two limits on that:

- **The landscape is grim but quiet.** The player reads the terrain to choose
  where to defend, and later thousands of units will move across it, so the
  ground must never compete with what stands on it. The mood comes from colour
  and light, not from clutter.
- **Only the objects listed here.** The things that define plague-era pictures
  (plague doctors, death carts, pyres, crosses, crows, the dead) are not
  scenery the game has a place for today: the renderer knows only the eleven
  sprite families below. Do not add them to this batch. They are the motifs
  the art for the horde and the colony should draw on once those are designed.

The paintings are a reference for mood, light and palette. Do not copy any
specific artwork, and do not imitate the look of *They Are Billions* (which
this game takes its gameplay from) or of any other game.

### Palette

All 47 images are painted from this one small palette and mixes of it.

| Name | Hex | Used for |
|---|---|---|
| Soot | `#1A1610` | deepest shadow, dark edges |
| Soot green | `#22332A` | conifers, deep shade |
| Umber | `#6A5646` | earth, trunks |
| Sallow olive | `#7C7E48` | grass and foliage |
| Ash | `#8A857A` | stone, gravel |
| Straw | `#A68E46` | dry foliage, tufts |
| Bone | `#D8CFB4` | highlights, pale stone, sand |
| Sulphur | `#C8A83C` | accent: sick foliage, flowers |
| Dried blood | `#8E2C28` | accent: iron rust, flowers |
| Tarnished gold | `#D4A62A` | accent: gold ore only |

- Nothing is more saturated than the three accents, and nothing is pure white
  or pure black. Bone is the lightest value; soot is the darkest.
- No clean blue and no fresh green anywhere.
- The accents are rare on purpose. Dried blood and gold are the two colours
  the eye should find first on any screen, so they are reserved for things the
  player needs to notice.

### Rules every image follows

1. **Light comes from the upper left.** Lit faces are up and left, shaded faces
   are down and right, and cast shadows fall to the lower right. The shader
   uses the same light for the terrain (direction `(-0.50, -0.62, 0.60)`, with
   y pointing down the image), so a sprite lit from any other side will look
   pasted on.
2. **Painted in three or four values, with strong light and dark.** Each
   material has a shadow, a base and a light value and at most a sparing
   highlight, laid on as opaque brush strokes that follow the form: leaf
   clumps, rock faces. Values may blend softly inside a form, but the shadow
   side must be clearly darker than the lit side, sinking towards soot. That
   contrast is what makes these paintings look the way they do. No airbrushed
   gradients and no glossy highlights.
3. **Dark edges, not outlines.** There is no uniform line around an object.
   The shadow side ends in a dark edge of soot `#1A1610`, 1 to 2 px at
   authoring size; the lit side meets the ground with no line at all. The
   silhouette itself stays crisp: paint may be soft inside the shape, never
   feathered at its border.
4. **Readable when small.** The camera zooms out until the whole map fits and
   in to 128 screen pixels per tile; the default is 32. Check every image at
   8, 16, 32 and 64 pixels per tile. Each sprite must be recognisable from its
   silhouette alone at 16. Brushwork blurs away when small, so the big light
   and dark shapes have to carry each sprite by themselves.
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
  mipmaps and drawn with premultiplied alpha. Slots have 16 px transparent
  padding and alignment, and the mip chain stops at level 4 to keep neighbouring
  sprites out of the filter when zoomed out. Each sprite has an **anchor**:
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
  no single big stone, no dark patch, no long crack. Keep
  detail even across the whole image.
- **Flat lighting.** No directional light, no gradients, no baked shadows of
  other objects. The shader adds all of that. The one exception is small
  pebbles, which may have a lit upper-left edge and a darker lower-right edge.
- **Stay inside the palette.** The shader multiplies these colours, so a
  texture that is much brighter, darker or more saturated than its palette
  shifts the whole map. Keep the average colour between the two values given.
- **Low contrast.** Roughly ±10% around the average. The ground is a backdrop.
- **Painted grain.** Detail is made of small dabs and short strokes of
  slightly lighter and darker paint over a flat base. Keep the marks small
  (1 to 4 px), evenly spread and faint. No dark edges on the ground.

### Grass

Open, buildable ground and the most common thing on screen.

- **Palette:** `#6E7442` to `#8C8A50`, a sallow olive.
- **Content:** thin, withered meadow grass. Soft mottling a few tiles across,
  and short vertical strokes that read as dry blades. No flowers, weeds or
  stones; sprites add those.
- **Watch for:** the shader shifts grass towards straw on dry ground and
  towards grey-green on wet ground. Keep the texture between the two, and keep
  it clearly greener than dirt: at far zoom the difference between grass and
  dirt is the difference between olive and brown.

### Forest floor

The ground under trees. Mostly hidden by canopies, visible in gaps, at the
edges of groves, and everywhere at far zoom.

- **Palette:** `#2E3324` to `#454430`, with patches of rotting leaf litter
  `#4E3E28`.
- **Content:** dark, damp ground under a dying wood: leaf mould, moss, litter.
  The most densely textured of the set. No trunks, roots or logs.
- **Watch for:** it must be clearly darker than grass so that groves read as
  dark masses when the tree sprites are not drawn.

### Dirt

Bare earth: buildable ground interleaved with grass, and the apron around
boulder outcrops and mountain feet.

- **Palette:** `#6A5646` to `#8A7460`, a grey umber.
- **Content:** dry, cracked, ashen earth with fine grain and a light scatter of
  small pebbles (2 to 5 px across, about one every 15 px). Cracks are
  hairlines no longer than half a tile.
- **Watch for:** keep it brown and matte. It must not read as sand (paler,
  more bone) or as rock (greyer).

### Rocks

Gravel under a boulder outcrop. A boulder sprite always stands on each of these
tiles, so this is only the ground between and around the boulders.

- **Palette:** `#6C665C` to `#8A8478`, ash grey.
- **Content:** broken stone and coarse gravel, denser and greyer than dirt,
  with pebbles of 3 to 6 px about every 10 px, roughly twice as many as on
  dirt.
- **Watch for:** no stone large enough to be mistaken for a boulder.

### Sand

The shore of a lake: pale silt and dried mud more than a beach.

- **Palette:** `#B8AC8C` to `#D0C6A6`, bone.
- **Content:** smooth pale silt with very faint ripple lines running roughly
  left to right, and a rare tiny pebble. The quietest texture of the set and
  the lightest thing on the map.
- **Watch for:** ripples must stay faint (2 to 3% brightness). Strong lines
  make the 8-tile repeat obvious.

### Mountain

Rock grain only. The shader supplies everything else about a mountain: its
shape, light and shade, and its colour from the foothills to the summits. The
same texture is reused at twice the density for cliff faces, where it is
brightened 2× and tinted warm.

- **Palette:** neutral grey centred on `#84827E`, close to 50% brightness.
  This one stays neutral whatever the rest of the palette does.
- **Content:** fine stone grain, hairline cracks and thin near-horizontal
  strata lines.
- **Watch for:** no hue, and nothing brighter than about 55% grey, because
  cliffs double the value and anything brighter clips to white. It has to look
  right both at one repeat per 8 tiles (mountains) and one per 4 tiles
  (cliffs).

### Shallows

The bed of a river ford, seen through murky water. The shader blends this 42%
towards its water colour and lays moving light patterns over it.

- **Palette:** `#6E7868` to `#90967E`, a grey-green silt.
- **Content:** a river bed of rounded pebbles (3 to 6 px) in silt. Paint the
  dry bed only: no water, ripples or highlights.
- **Watch for:** give the pebbles a little more contrast than looks right on
  its own, because the water layer flattens it. A ford has to read as "you can
  cross here" next to deep water.

### Water (data, not a painting)

Leave this one generated in code. It is listed so nobody paints over it. Its
three channels are separate greyscale patterns that the shader scrolls against
each other: red and green are two different webs of thin bright caustic lines
on black, and blue is a soft ripple field centred on mid-grey. Water colour,
depth and foam are computed. The same applies to the ninth "noise" layer.

### Colours the shader owns

Some colours on screen are constants in the terrain shader
(`kTerrainFragment` in `game/render/map_renderer.cpp`), not pixels in any
image. They are named at the top of the shader and tuned by eye against the
painted textures:

| What | Constant | Colour |
|---|---|---|
| Water at the shore, and the wash over fords | `WATER_SHORE` | murky grey-green, about `#5E7569` |
| Water mid-depth | `WATER_MID` | stagnant teal-grey, about `#38524C` |
| Deep water | `WATER_DEEP` | near-black green, about `#1A292B` |
| Light patterns on water | `WATER_GLINT` | faint, tinted bone |
| Shoreline foam | `FOAM` | dirty scum, about `#C7C4AB`, faint |
| Scrub on mountain foothills | `SCRUB` | sallow olive, as grass |
| Smoke in the air | `HAZE` | warm grey, about `#8F8778` |

The shore colour is a little darker than a straight match to the palette would
give, so that a ford still reads as paler than the river around it.

The leaden sky and smoke haze of the reference paintings cannot come from the
images, so the shader ends with a slight overall grade: the ground loses about
a seventh of its saturation and a thin layer of `HAZE` is laid over it. Sprites
are not graded; they are painted in the palette already.

The moisture tint on grass, the warm tint on cliff faces and the paler tone of
high ground are unchanged from the placeholder version.

## Sprites

### Shared requirements

- One PNG per sprite, RGBA, sRGB, **straight (not premultiplied) alpha**, at
  exactly the canvas size listed. The atlas packer premultiplies.
- Transparent background with at least 1 px of fully transparent margin on
  every side. No stray semi-transparent pixels away from the object; they show
  up as halos once the atlas is mipmapped.
- The object's base (the bottom of a trunk, the middle of a rock's footprint)
  sits on the **anchor** pixel listed, measured from the top-left corner.
- **Contact shadow painted into the sprite:** a soft ellipse of soot `#1A1610`
  at 35 to 45% opacity in the middle, fading to nothing at its edge, lying on
  the ground under the object and pushed slightly to the lower right. It is wide and flat, about
  2.5 times wider than tall. Tufts, flowers and reeds have no shadow.
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

- **Shape:** a gnarled, sickly tree. The crown is a ragged mass of five to
  seven leaf clumps, slightly wider than tall, with a few bare, crooked branch
  tips breaking out of its silhouette. The trunk is short, dark and twisted, and
  shows for about a fifth of the height.
- **Shading:** three values and a sparing highlight, with the lightest clumps
  at the upper left. The lower-right clumps fall into deep shadow.
- **Palettes:** three, two variants each, so a forest is a patchwork of sick
  colours and not one flat tone.

  | | Shadow | Base | Light | Highlight |
  |---|---|---|---|---|
  | A, sallow olive | `#2A3020` | `#4A5230` | `#6E7440` | `#96965A` |
  | B, jaundiced ochre | `#3A3018` | `#665426` | `#927A36` | `#B8A254` |
  | C, blighted rust | `#34201A` | `#5E3626` | `#8A5234` | `#AE7A4C` |

  Trunk `#4A3A2C` on the lit side, `#2A2019` in shade.
- **Variants:** variants 0 to 2 keep a full crown. Variants 3 to 5 are further
  gone: a thinner crown with more bare limbs showing, but still a solid mass.
- **Watch for:** the canopy is the main thing. It has to look good as a mass of
  thirty overlapping crowns and it has to look impassable, so no variant is a
  bare skeleton. Keep each crown's darkest tone at the bottom edge, where the
  tree in front will overlap it.

### Conifer

The tree of higher, drier ground. Forests shift from broadleaf to conifer with
altitude, and the two mix along the boundary.

- **Shape:** a gaunt, narrow spire of four stacked tiers of drooping boughs
  with a ragged, saw-toothed lower edge, on a short trunk. A dead lower bough
  or two may hang bare. Clearly taller and narrower than the broadleaf, with a
  pointed top.
- **Shading:** three values. Light on the left of each tier, and the underside
  of each tier in deep shadow so the tiers separate.
- **Palette:** shadow `#141E1A`, base `#22332A`, light `#3A4C38`. Dark edge
  `#0C1210`. Nearly black, and the only cold colour on land, so the two kinds
  of forest can be told apart at a glance.
- **Variants:** vary the lean of the trunk by a pixel or two and the raggedness
  of the tiers.

### Bush

A low thicket scattered on open grass. Units walk through it and buildings go
over it.

- **Shape:** a tangled mound of bramble, wider than tall, with three or four
  thin leaf clumps and a few thorny stems arching out of it. No visible trunk;
  it sits directly on the ground.
- **Palette:** shadow `#2C2E1E`, base `#4A4A2C`, light `#6C693C`, highlight
  `#8E8850`. One or two variants may carry a few berries in dried blood
  `#8E2C28`, 1 to 2 px each.
- **Watch for:** it must not look like a small tree. Keep it to about half a
  tile tall, with a lighter shadow than a tree's.

### Stone, iron and gold deposits

The three resources on the map. A deposit is a cluster of tiles with one of
these sprites on each, so the sprites sit shoulder to shoulder and have to
work as a group. Telling the three apart instantly, at any zoom, is the most
important readability requirement in this brief.

All three share one form: a pile of three or four angular, faceted rocks with
flat tops, seen slightly from above, the nearer rocks overlapping the further
ones. Each facet is one flat stroke of paint, lit or shaded as a plane. They
differ in colour.

- **Stone:** pale, chalky, bone-grey rock, `#A6A29A`, with lit facets close to
  bone `#D8CFB4`. The cleanest and brightest of the three.
- **Iron:** dark, almost black-brown rock, `#4E3A36`, streaked and blotched
  with rust the colour of dried blood, `#9E4424`, over about a third of its
  surface.
- **Gold:** plain ash-grey host rock, `#6E685E`, with veins and nuggets of
  tarnished gold `#D4A62A` over about a fifth of its surface and a few pale
  glints `#F2E08A`. This is the only use of gold in the whole set.
- **Watch for:** stone must not be confused with the `boulder` family, which
  is scenery. Deposits are paler, cleaner and more angular. Gold must stay
  visible when the sprite is 12 px wide, so use a few bold patches of yellow,
  not fine speckle.

### Boulder

One or two large weathered rocks: the obstacle on an outcrop tile. These are
the small features a colony has to build around.

- **Shape:** one big rounded-angular boulder, in some variants with a smaller
  one in front of it. Heavier, rounder and darker than a deposit.
- **Palette:** dark weathered grey-brown `#6E685C`, blotched with sickly
  lichen `#8A8A4E`.
- **Variants:** two with a single boulder, two with a pair.

### Tuft

A fan of dry grass blades. Decoration on grass, and sparsely on dirt, where the
engine tints it warmer.

- **Shape:** five to seven thin blades spreading from one root point, the
  outer ones shorter and leaning outward, some bent or broken.
- **Palette:** blades between `#5A5A30` and `#9A9258`, drier and more straw
  than the grass under them. No dark edge and no shadow; at this size they
  would swallow the sprite.

### Flower

The tuft with flower heads on about half the blades. Flowers grow in drifts
that become meadows, so one sprite is seen in groups of dozens.

- **Shape:** as the tuft, with small heads about 3 px across, some drooping.
- **Palette:** each variant uses one petal colour: bone `#E4DCC4`, sulphur
  `#C8A83C` or dried blood `#8E2C28`. Four variants, so one colour appears
  twice with a different arrangement.
- **Watch for:** the petals are the most saturated pixels on the ground. Keep
  them small, and keep the dried-blood variant the rarest-looking: fewer heads
  than the others.

### Reed

Tall waterside grass. Appears only on grass tiles that touch water, so it
draws the shoreline.

- **Shape:** five to seven tall, nearly upright blades, more than twice the
  height of a tuft, with slim dark seed heads on a few of them. Dry and
  brittle.
- **Palette:** blades between `#4E4E30` and `#857E4E`. Seed heads `#3E2C20`.

### Pebble

Two or three small stones. Scattered on dirt, rarely on sand, and in fords,
where the engine tints them cooler and they read as stones standing out of the
water.

- **Shape:** small rounded stones, flat and low, with a faint shadow.
- **Palette:** ash `#8A857A`, dark edge `#36322C`.
- **Watch for:** clearly smaller and flatter than a boulder. Nobody should
  think a pebble blocks the way.

## Delivery

The viewer loads the art at start-up from this layout, under `./assets` or the
directory given with `--assets`:

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

The loader is strict about what the renderer depends on. The seven ground
textures are used only if all of them are present and 256 × 256, and the forty
sprites only if all of them are present with exactly the canvas sizes above;
otherwise that whole group falls back to the placeholders and the log names the
first file that was wrong. Anchors are fixed in code (`kFamilies` in
`terrain_art.cpp`), so a sprite must be painted to its anchor, not the other
way round.

### Checklist before handing back

- [ ] 7 ground textures, each 256 × 256, opaque, and seamless when tiled 3 × 3.
- [ ] No ground texture shows a repeat when tiled 6 × 6 and viewed at 25%.
- [ ] Each ground texture's average colour lies inside its palette range.
- [ ] `mountain.png` is neutral grey with no pixel above about 55% brightness.
- [ ] 40 sprites with the exact canvas sizes and file names listed.
- [ ] Every sprite's base sits on its anchor pixel.
- [ ] Every sprite is lit from the upper left, with its shadow to the lower
      right.
- [ ] Every colour comes from the palette or a mix of it: no clean blue, no
      fresh green, no pure white or black.
- [ ] Dried blood and gold appear only where this brief puts them.
- [ ] Every sprite has a clearly darker shadow side, no uniform outline, and
      still reads correctly at 16 px per tile, where the brushwork has blurred
      away.
- [ ] Stone, iron and gold can be told apart at 16 px per tile.
- [ ] Blocking objects (trees, deposits, boulders) look solid; walkable ground
      cover does not.
- [ ] Nothing is depicted beyond the eleven sprite families and seven ground
      textures listed.
- [ ] A contact sheet of all sprites on the grass texture, at 1× and 2×, is
      included for review.
