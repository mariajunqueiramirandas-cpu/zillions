# Gathering and the first camp

The first economy runs alongside the existing character and management cameras.
The player begins with an empty 20-slot bag. Resources stack to 50, chests to
one, and the two tools occupy equipment state outside the bag. Each camp chest
has 24 slots. Crafting uses carried supplies, so returning to storage matters.
This milestone is session-only: quitting or Ctrl+R resets the economy. Ordinary
R no longer accidentally replaces the map during character play.

## Play loop

Map startup places branches, pebbles and fiber bushes on reachable open ground
near the colony start. Branches yield four wood, loose stones three stone and
bushes/reeds four fiber. Each hand-gathering cycle takes 0.45 seconds and awards
one item. The generated scenery's existing trees, boulders and stone deposits
also become resource nodes. Iron and gold await the processing/upgrades tier.

| Recipe | Cost | Craft time |
|---|---|---|
| Rope | 3 fiber | 0.6 seconds |
| Stone axe | 3 wood, 2 stone, 1 rope | 1.5 seconds |
| Stone pickaxe | 3 wood, 4 stone, 1 rope | 1.5 seconds |
| Chest | 12 wood, 2 rope | 2 seconds |

Hold left mouse near an object's ground position to gather. An axe extracts ten
wood from a tree, one per 0.85-second strike. A pickaxe extracts twelve stone
from a boulder or thirty from a deposit, one per 0.95-second strike. Movement,
release, a changed target, opening inventory or changing camera mode interrupts
the current strike. Full bags stop work without consuming another unit. Tools
are selected automatically for the resource and are permanent in this version.

Reach checks follow terrain levels, ramps and intervening solid objects. They
do not depend on camera angle. Depletion changes the existing scenery object
into a stump or rubble, removes its collider and updates the tile's terrain and
construction flags after every solid object on that tile has been cleared.
Ordinary asset reloads cannot restore harvested resources.

I/B opens inventory and crafting. Left-click picks up, merges or swaps stacks;
right-click takes half or places one item. Shift-click queues up to five crafts,
stopping when ingredients run out. Output and ingredient checks are repeated at
completion: supplies moved away during crafting cannot produce free items or
lose ingredients. Both tools are equipped when made and cannot be crafted twice.

Choose Place chest, inspect the translucent preview, and left-click nearby clear
ground. Right-click/Esc cancels. The chest has a solid footprint; it cannot be
placed over the character, another obstacle, a ramp or an inaccessible level.
Click a nearby chest to open storage. Shift-click transfers to the other
container; right-click with Shift transfers one. Held stacks must be placed or
returned before closing if there is no room to return them automatically.

## Art and animation

The tools are procedural three-dimensional shapes in the character's body frame.
Wooden shafts, stone heads and binding bands are added before the body is
painted. A shared tool axis supplies both hand grips, and two-bone arm posing
keeps the limbs attached. The work cycle lifts, strikes and recovers; hand
gathering has a separate reaching motion. The same renderer handles camera
rotation, material shading, painted filtering and terrain occlusion. These are
live posed volumes, with no imported mesh or sprite sheet dependency.

Existing painted trees, rocks, bushes and ground textures are reused. Four new
props (fallen branch, stump, rubble, chest) are authored in
`game/render/terrain_art.cpp`, supersampled and packed into the existing padded
atlas. Their umber, ash and bone colors follow `docs/art-direction.md`. Icons
are simple authored silhouettes in `survival_ui.cpp`, with the same materials.
Small chips and floating gains provide strike feedback. Audio is a later pass.

This is the first art pass. Further art should be delivered in small groups:

1. Refine the axe/pickaxe silhouettes and the grip/strike animation against a
   tree and stone deposit, at normal play zoom and close up.
2. Paint final camp props and resource icons from those approved shapes. Keep
   transparent masters, export dimensions and ground anchors together. Props
   currently use the scenery billboard path; an asymmetric building should use
   four directional paintings or a dedicated geometry path so rotation reveals
   the correct face.
3. Define workbench, storage and basic construction silhouettes before drawing
   their finished art. Review one representative building in the actual scene
   before producing variants.
4. Reuse the rig/material system for future colonists. Give zombie types their
   own approved silhouettes and movement cycles; choose sprite baking or live
   rendering from measured horde costs before producing the full animation set.

For each group, check palette, matching light, transparency, anchors, shadows,
all four camera angles and readability at 8/16/32/64 pixels per tile. Keep source
art alongside exports and use F5 for scene review. Inventory text, quantities
and progress bars remain drawn by the UI, allowing text/layout changes without
repainting assets. No new third-party assets or licenses are introduced here.

![Inventory and recipes](screenshots/survival-inventory.png)

![Chest storage](screenshots/survival-storage.png)

![Procedural axe and camp props](screenshots/survival-tools.png)

## Implementation and validation

`survival/survival.*` is headless. Resource nodes retain stable scenery indices,
and a tile index makes mouse targeting local. `rebuild_scenery_colliders()`
updates physical volumes without regenerating scenery. `refresh_objects()`
updates tile data, scenery order and shadow casters without rebuilding mountain
relief. Save/load must eventually serialize this mutable state rather than
recreate it from the original seed alone.

```sh
cmake --build build -j 5
ctest --test-dir build --output-on-failure
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_survival_render_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_render_tests
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/tests/zl_figure_render_tests
```

The survival suite checks stack conservation, split/transfer behavior, recipe
transactions, full bags, tool gates, cancellation, depletion/collision,
multi-object tiles, bootstrap supplies across seeds and resolutions, chest
placement, tool poses and a complete empty-bag-to-stocked-chest simulation.
The GPU suite clicks actual HUD hit boxes to craft both tools and a chest,
splits/returns a held stack, transfers supplies and renders props/tools at all
four camera angles. Existing terrain and figure suites remain applicable.

Set `ZL_SURVIVAL_CAPTURE_DIR=/tmp/survival-review` on the survival GPU command to
write the review frames. Tool poses can also be inspected independently:

```sh
./build/game/zillions --seed 7 --size 256 --resolution 1 --zoom 180 \
  --tool axe --work-phase 0.4 --screenshot /tmp/axe.png
```

A live X11 input check also gathers the three starter materials, crafts rope
and an axe through the UI, then switches to management mode and rotates the
camera. F2 captures the current framebuffer, including the gameplay HUD.
