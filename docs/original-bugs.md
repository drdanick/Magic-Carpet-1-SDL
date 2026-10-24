# Bugs in the original game

Bugs found in Magic Carpet's own code (the DOS `CARPET.EXE`), not introduced by the port. Each was
confirmed by running the original machine code on the same game state (`tools/diff_tick.py`,
`tools/diff_call.py`): the original behaves the same way.

Status:
- **kept**: the port behaves exactly like the original;
- **worked around**: the port avoids a crash or memory corruption that DOS tolerated, and otherwise
  behaves like the original;
- **fixed**: the port deliberately behaves differently (marked `// PORT:` in the code).

| # | Bug | Where | Status |
|---|-----|-------|--------|
| 1 | Stuck explosion after a worm segment dies | `UpdateFollower_1A390` | kept |
| 2 | Rotated sprites drawn outside the frame buffer | `sub_2C410_2C450` | worked around |
| 3 | AI wizard measures its distance to a missing target | `AiApproachTarget_15470` | worked around |
| 4 | Reads/writes through null or stale pointers | several (see below) | worked around |
| 5 | Level performance screen draws with a freed font | `ShowLevelPerformance_4E5B0_4E8F0` | worked around |
| 6 | Terrain smoothing reads in front of the map on its first row | `sub_360C0_36480` | fixed |
| 7 | Castles built among walls: the site check looks at one quarter of the site | `IsCastleSiteFree_12F70` | kept |
| 8 | Full entity table recycles live entities (balloons vanish, HUD divides by zero) | `NewEvent_372C0_37680` | fixed |

## 1. Stuck explosion after a worm segment dies (kept)

**Symptom:** an explosion sprite stays in the air forever, e.g. near the end of a level where a worm
was killed (seen on level 15).

**Cause:** worm segments are a chain. Each segment keeps its leader's entity index at entity offset
+52 (`var_u16_29847_52`) and its follower at +54. When a segment's life drops below zero,
`UpdateFollower_1A390` (the segment's update) sets its *leader* to the dying state
(`sub_424F0_42830(leader, a2 + 2)`, state 20 for these segments) and clears the leader's +52. It
never checks that the leader still is a segment. If the leader died in an earlier tick, its slot is
free, and the game may have reused it, e.g. for an explosion (class 10, model 0) created in the same
fight. The explosion is then put into state 20, whose handler (`0x262C0`) is an empty `ret`: it
never changes again and stays drawn.

**Observed:** frame 36216 of the level 15 recording: segment 174 dies (life -2200) while segments
157 and 175 still name it as their leader. Frame 36217: slot 174 is reused by an explosion. Frame
36218: segment 157 dies and sets entity 174 (now the explosion) to state 20. Every tick and the
drawing match `CARPET.EXE`.

**Possible fix:** in `UpdateFollower_1A390`, only change the leader if it still is a creature of
the follower's class and model (and alive), and otherwise just clear the follower's +52. The same
applies to the second branch (follower hit by a wizard, `class == 3`) and to the leader link read in
the `switch` on the leader's state. Mark the change as a deliberate deviation with `// PORT:`.

## 2. Rotated sprites drawn outside the frame buffer (worked around)

The rotated-sprite path of the sprite blitter (`sub_2C410_2C450`) does not clip rows against the
frame buffer when a sprite is very close to the camera in 640x480, and writes outside the screen
buffer. DOS4GW let this scribble over neighbouring memory; in the port it crashed (levels 5 and 6).
The screen buffers have guard bands (`MallocGuarded`) and the blitter's frame accesses (`FbPut` /
`FbGet`) drop anything outside the buffer. What is drawn on screen is unchanged.

## 3. AI wizard measures its distance to a missing target (worked around)

`AiApproachTarget_15470` computes the distance to its target (`a2 + 0x48`) before checking whether
there is a target; `AiBuildCastle_138F0` calls it with none, so it reads address 0x48. DOS ignored
the read and the result is unused. The port skips the measurement without a target (level 7 crash).

## 4. Reads/writes through null or stale pointers (worked around)

DOS4GW maps low memory, so the original silently read/wrote through null or stale pointers, e.g. the
animation sequence at the end of a level and the player slots of freed or empty wizards in the
main-menu demo. The port guards these with `// PORT:` checks that mirror what the data means (e.g. "is
a live wizard") and otherwise behaves the same.

## 5. Level performance screen draws with a freed font (worked around)

`ShowLevelPerformance_4E5B0_4E8F0` frees its font (`sfont2`) when a key is pressed and then draws the
screen once more with it. DOS's memory pool kept the data, so nothing was visible; the port frees the
font after the loop instead.

## 6. Terrain smoothing reads in front of the map on its first row (fixed)

`sub_360C0_36480` smooths a tile's height only if the terrain types of the tiles at -257, -256 and -1
(the neighbours above-left, above and left) and of the tile itself are all "open" (type 0..5 or
above 0x22). The original computes those indices as `(uint16)tile - 257` etc. in 32 bits
(`mov dl, [ebx + 0xcc0df]` with `ebx` the 16-bit tile index), so for tiles on the map's first row
(tile < 257) it reads up to 257 bytes in front of the terrain map: DOS variables of the sound code.
The 3x3 averaging loop right after it wraps around the map with 16-bit arithmetic, which is clearly
what was meant. Found from the disassembly while porting to Linux: the bytes in front of the array
depend on the compiler's layout, so GCC builds smoothed different first-row tiles than MSVC builds.

The port wraps those three reads around the map, like the averaging loop. (During the 64-bit port it
read what the 32-bit MSVC build found there, the end of `mapEntityIndex`, to keep the old recordings
valid.)

## 7. Castles built among walls: the site check looks at one quarter of the site (kept)

**Symptom:** on level 12 the purple wizard's castle stands in the maze in the north-west of the map,
between impassable walls, and never grows.

**Cause:** an AI wizard does not look at the terrain when it picks a castle site. `AiChooseCastleSite_13F00` takes
points from a fixed grid of 64x64-tile blocks: each block's corner or the point 31 tiles in, starting
with the block the wizard is in. It picks the first point with no castle within 12288 units. On
level 12 the grid point (192, 64) lies in the maze. The AI castles in the recording stand on such
points: (0, 64), (0, 0) and (192, 64).

The castle spell's missile then stops at the first point of its path where
`IsCastleSiteFree_12F70` fails and builds one step back. That check tests only the 8x8 tiles from
(x-8, y-8) to (x-1, y-1), i.e. the quarter of the site north-west of the castle point, for walls
(`mapAngle` bit 0x80). The missile also builds without any check where it hits the ground, hits its
target or runs out of time. A castle can therefore stand right next to walls, and
`CanCastleGrow_12D10` (which does see the walls) then keeps it at its first size.

**Confirmed:** `possible_enemy_castle_bug.recording` (a user recording): every tick from frame 3060 to
3120 matches the original byte for byte, including frame 3116, where the castle is created at tile
(192, 64). The 8x8 window is the original's own loop (`sar`, `sub al, 8` at 0x1302D).
`tools/diff_funcs.py --func AiChooseCastleSite_13F00` gives identical results for wizards 105 and 109 on eight
snapshots between frames 1300 and 6000, including frame 3100, where wizard 109 picks (192, 64).

## 8. Full entity table recycles live entities: castle balloons vanish, HUD divides by zero (fixed)

**Symptom:** on level 19 the game crashes (integer divide by zero) while lightning bolts are flying
around; just before, two of the player's castle balloons have disappeared.

**Cause:** the game has 999 entity slots. When the free list is empty, `NewEvent_372C0_37680` takes a
slot from a second list of "recyclable" entities (flags `0x20400`) and overwrites it. That list is
only rebuilt now and then (`sub_37220_375E0`: level start, wizard rebirth, castle code). In between,
listed entities die and their slots get reused by entities that must not be recycled. The original
recycles them anyway. On level 19 the list held both slots of the player's balloons (class 3
model 3), a mana ball (class 10 model 39) and other live entities. A lightning bolt
(`LightningBoltTick_535E0_53920`) needed so many new entities that it reached those entries and
turned the balloons into its own pieces. The castle's balloon slots (`Type_160::var_52`) still named
them. The HUD (`sub_22E50`, 0x23531) then divided by the new entity's max mana, which is 0.

**Confirmed:** `level19.recording` (a user recording): ticks 12400-12598 match the original byte for
byte. Run on the port's state after tick 12598, the original's `DrawGameFrame_20FB0` faults on the
same `idiv` (`tools/diff_call.py`). The free list is empty and 9 of the 155 recycle entries are
stale.

**Fix:** `NewEvent_372C0_37680` skips recycle entries whose slot now holds a live entity without the
recyclable flags (`// PORT:`). This only changes the game when the entity table is full.
