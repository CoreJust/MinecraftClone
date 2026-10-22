# Roadmap to the next versions (estimated, up to changes and elaboration)

Current version family: EarlyDev 0.1.0 - Initiation. This is the canonical product scope, not a completion ledger.

Both development lines share this plan. See [AI delivery sequencing](ai/ROADMAP.md) for dependencies and acceptance, [backlog](ai/BACKLOG.md) for actual task state, and [version history](<version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md>) for snapshots. Tentative choices and inconsistent stage labels below remain explicit decisions; do not silently reinterpret them.

Planned path to it (per snapshot):

1. The first runnable version of the game (without any content at all).
2. Network test and early project structure outline. The game can run a server and several clients, client have players 2x2 in a 32x32 flat world and generate random movements, the server handles the collision. Rendering is implemented via console.
3. Rendering test. The logic stays the same, but now it is rendered within a window with real Vulkan graphics.
4. 3D and controls test. The world becomes 3D (although flat in logic), the players can be real or bots, the real ones have a working camera and WASD movement. The world has a grid below and simple colored box players.
5. Blocks test. The world consists of a single chunk with only 2 block types: air and stone. The player flies around it.
6. World test. The world is full 65'536 x 65'536 x 1'024 blocks. The chunks are still only air and stone with a simple generation pattern.
7. Entity permissions and physics test. General permissions apply to every entity, with hard restrictions and overridable soft defaults; players are the first user. Permission-gated flight and phasing coexist with gravity, jump and collision. The snapshot adds a deterministic 16-color player parallelepiped with first-person, rear and front-facing third-person views, adverse-network acceptance, full-game performance checks including the x200 speed modifier, and automated fail-closed sanitizer/static-analysis gates.

## EarlyDev 0.1.1 — BasicBlocks

Owner-supplied plan, recovered and recorded on 19 October 2026, consolidated
into four snapshots on 22 October: S3 joins S2, and S5/S6 become the new S4.
This replaces the earlier fixed-pattern outline and the assistant's proposed
reliability/water snapshot splits. Fixes accompany the product snapshots;
they are not separate product features or snapshots. The inherited cursor,
moving-performance and terrain-loading defects are the first engineering batch
alongside S1, with their regression checks retained throughout the minor.
See [S1 design and acceptance](ai/decisions/earlydev_0_1_1_s1.md) and
[delivery sequencing](ai/ROADMAP.md#basicblocks-snapshots).

1. **Terrain-v2 world generation and solid blocks.** The owner finalized the
   [Terrain-v2 design](ai/decisions/earlydev_0_1_1_s1_generation.md) for S1;
   Terrain-v1 is the current repeating-wave generator. Add grass, dirt, bedrock,
   snow and ice alongside existing stone. Place a one-block-deep bedrock layer
   at the bottom. The owner's later clarification makes seeded tectonic
   simulation or a credible approximation the foundation of the base heights.
   Large, medium and small continents, mountain ranges, ocean trenches,
   islands and peninsulas should emerge naturally, with their distribution
   decided by the seed. Support smooth, curved irregular and ragged coasts;
   elongated large islands with satellites, large-island groups, medium/small
   archipelagos, isolated oceanic islands/groups and subcontinental islands.
   Geography is anisotropic: land may concentrate in one half-world, and sparse
   unusual landscapes arise from the processes. Do not distribute one shape
   uniformly or require every feature in every seed. Add detail only where needed; the earlier
   `exp(local_octave_noise()) + continental_octave_noise()` suggestion no longer
   defines the large-scale foundation. Mountains should mostly form ranges
   with lower mountains, foothills and plains around them, while allowing
   occasional isolated mountains. Choose
   `SEA_LEVEL` so roughly half the terrain is above and half below it.
   Global preparation builds tectonics, then terrain/water elaboration
   (erosion, coasts, ranges/trenches, islands/peninsulas, seas/lakes, large
   rivers and glacial inlets), then climate from winds and ocean currents.
   Progressive first-touch regional elaboration precedes local chunk generation.
   Climate-driven materials apply in S1, superseding fixed outer-20% polar
   bands. At or above sea level, use one topmost block, four ground blocks,
   then stone down to bedrock. Below sea level, only terrain in
   the four-block band immediately below `SEA_LEVEL` uses ground material;
   deeper terrain is stone. Add standalone world-map generation for tests and
   demos with ordered seeds `[0, 1, 42, 777, 2004, 8800555]`; the global atlas
   also provides the future unloaded-map detail level. During initial loading,
   show the actual generated map, refine safe spawn, generate a 48-block sphere
   and show **Press any key to start** within 4 s. From the same original load
   request, fill the playable 20-chunk/320-block sphere within 20 s on Mac or
   15 s on Windows; key wait does not reset the deadline. At x30 preserve that
   radius with ≥99% of moving time at ≥120 fresh FPS on Mac or ≥300 on Windows,
   using existing qualification settings. Use CoreLang 0.1.5 with JIT/AOT,
   temporarily pinned to an exact owning-chat ai-dev commit, then its release;
   new language features belong to later versions. Add normal-game 2D tests
   that replace rendering with in-memory top-block-color pixels while retaining
   actual generation. Require connected new chunk coverage, deterministic
   Python/native pixels and order/movement-independent worlds under the same
   version/seed/configuration. Review module separation and subsystem-specific
   correctness/performance modes. Qualify two physical laptops with one local
   and one remote player, hosting on Mac and Windows. Detailed contracts are in
   the S1 plan. Physical water itself arrives in S3.
2. **Placing, breaking, game modes, HUD and maps.** Highlight the reachable
   block under the player's gaze. Breaking shows cracks and cubic particles. Normal break
   times are snow 1 s, dirt/grass 1.5 s, ice 2 s and stone 3 s. Bedrock cannot
   be broken normally; the owner confirms Creative can break it. Breaking permissions provide a block blacklist or
   whitelist, including an empty list, and normal or effectively instant speed.
   Spectator grants flight and block passthrough with an empty breaking
   whitelist. Creative derives from Spectator, disables block passthrough,
   uses an empty blacklist and quick destruction. Survival derives from
   Creative, disables flight, blacklists bedrock and uses normal destruction speed. The HUD
   prints the mode and precise permissions in parentheses. B cycles the block
   selected for placement, initially displayed in the HUD. Creative bedrock
   breaking and Survival without flight were confirmed on 22 October 2026.
   The [S2 design](ai/decisions/earlydev_0_1_1_s2.md) defines interaction,
   world/replication ownership, rendering and future acceptance. S2 also owns
   the replacement terrain model deferred from S1, subject to the separate
   research's owner-selected, qualified handoff.
   Add a central crosshair for the edit ray. HUD/crosshair style supports a
   single color or a dark/light pair.
   Adaptive color selection depends on the scene behind each individual pixel,
   so an element remains readable across both snow and dark terrain; do not
   switch the entire element to one color based on an average background.
   Add a gameplay minimap and interactive world map, brought forward from
   GeographyDev by the owner on 22 October 2026. Use the shared generated
   world and canonical edits; detailed map behavior remains in the S2 design.
   Improve pre-connect loading, generation, game and world-map views with
   existing text/background/map rendering. Use a HUD-free game capture at
   loading and the accepted tectonic/relief/water/ice/climate/arrival sequence,
   keeping progress/readiness honest and final ice/geography consistent.
   A quieter HUD shows loaded chunks, memory and local climate (temperature,
   humidity and salinity only where applicable). Minimap covers a local L1/L2
   area with N/E/S/W. The owner accepted the prototype at `232eae68`: uniform
   colored HUD readouts without View text (F5 retained), full-viewport map and
   independent translucent climate layers. No widget framework is required.
3. **Water.** Fill empty blocks at or below `SEA_LEVEL` with semitransparent
   water. Include swimming, underwater effects with at least bluish screen
   postprocessing, and flowing water. Movement slows in water when neither
   flight nor passthrough is enabled. A swim permission, enabled by default in
   Creative/Survival, permits swimming upward. Water is not broken; placing a
   solid block into water replaces it. Research and elaborate fluid behavior
   when planning S3, including alternatives to Minecraft's water model.
4. **Player models, animation, multiplayer and bots.** Replace the box body
   with a conventional player model. First person shows two hands with distinct motions for
   running, jumping, breaking, placing and swimming. Rear/front external views
   show the complete animated model. Move the selected-block display from the
   HUD into the player's hand. Add opacity in the range 0–100%; Spectator uses
   40% opacity. Exact art and animation direction belongs to S4 planning.
   Every multiplayer player has the model.
   Bots wander, occasionally place/break nearby blocks and may swim, subject
   to their capabilities; bots with flight use it, and Spectator bots only fly.
   Keep bots near a player: proposed thresholds are return movement beyond
   64 blocks, temporary Spectator flight beyond 128, and acceleration up to
   500× beyond 160. Restore the original mode inside the usual 64-block range.
   Recover a valid nonintersecting position when leaving passthrough inside
   terrain; the same rule applies to human mode switches. Anchor-player
   selection and exact thresholds are decisions for S4 planning.

These are planned production outcomes, not delivered native features. S1's seeded
geography and climate-driven materials are the initial terrain foundation.
The richer biome content, sand, trees and polar ice-wall work listed for 0.1.3
remain later scope; its older temperature/height wording does not postpone
S1's explicitly requested climate simulation. Menus, inventories and save/open-world UI
remain in 0.1.2. Planning does not start implementation or release reporting.

Plans for EarlyDev 0.1.2 - GUI:
1. Main screen.
2. World creation screen / entering others' worlds screen.
3. Pause screen in the game.
4. Inventory hotbar (and inventories all in all).
5. Saving the world and opening it.
6. F3 debug info.

Plans for EarlyDev 0.1.3 - BasicWorldGen:
1. Global map generation (oceans, continents, etc) by height and temperature.
2. Local biomes (deep ocean, ocean, beach, plains, hills, mountains, etc).
3. Sand blocks.
4. Icy poles and the great wall of ice in extreme north/south.
5. Trees.

Plans for EarlyDev 0.1.4 - Sky&Graphics:
1. Some lighting system, maybe additional graphics effects.
2. Day/night.
3. Sun and moon, stars, clouds.
4. Latitude effects on sky position and some blocks color.

Plans for EarlyDev 0.1.5 - Crafting:
1. Inventory interface.
2. Craft recipes.
3. Workbench and chests, with own interfaces.
4. Planks, sticks, wooden tools (shovel, axe, pickaxe).
5. Blocks requiring specific tools and different drops (cobblestone).
6. Stone tools.

Plans for EarlyDev 0.1.6 - Chat:
1. Player nicknames.
2. Chat system.
3. A set of commands (turing-complete), permissions.
4. (For tests) Command blocks.

Plans for EarlyDev 0.1.7 - Ores:
1. Coal, copper, tin, iron ores.
2. Furnace and melting recipes.
3. Ingots (copper, tin, bronze, iron).
4. Metal blocks and tools.
5. Ore generation.
6. Cave generation.
7. Torches.

Plans for EarlyDev 0.1.8 - Mobs:
1. Health bar.
2. Fighting system.
3. Peaceful mobs.

Plans for EarlyDev 0.1.9 - Enemies:
1. Dead miners, zombies, bats, ...
2. Dying and respawning for players.
3. Armor (wooden, metal).
4. Swords, other weapons?.

That makes for EarlyDev complete after some additional tests and probable improvements, next will be GeographyDev 0.1.

Plans for GeographyDev 0.2.0 - Map:
1. Seas, rivers, lakes, ponds (+ gravel, ?).
2. Islands.

Minimap and world-map UI move forward to EarlyDev 0.1.1 S2; later geography
uses their shared world-data path.

Plans for GeographyDev 0.2.1 - SurfaceBiomes:
1. Deserts (+ sandstone and cactus).
2. Tundra (+ frozen dirt).
3. Taiga, step, savanna, jungles.
4. Better terrain.

Plans for GeographyDev 0.2.2 - Forests:
1. Better forest generation (more realistic tree structures and tree placements).
2. Meadows.
3. Younger, older trees, bushes, falled trees, dead trees.
4. Multiple tree types (e.g. pine, spruce, oak, birch, weeping willow, palm, acacia, maple, ginkgo, sakura, baobab, giant sequoia, banyan, tree-like cactus species, olives, juniper).

Plans for GeographyDev 0.2.3 - Caves:
1. Better caves generation (probably layers, different kinds of caves, rifts, e.g.).
2. World layers: surface, underground (dirt and some stone), caves (stone), deep caves (compressed stone?, occassional lava), nether (netherstone, lava bodies).
3. Improved ore generation per layer (specialized ore blocks).
4. Additional mineral blocks.

Plans for GeographyDev 0.2.4 - Underwater:
1. Weeds of all kinds.
2. Complex ocean terrain, new blocks.
3. Currents.

Plans for GeographyDev 0.2.5 - Sky&Weather:
1. Sky islands.
2. Seasons.
3. Rains, hailstorms, winds, snowfall, lightning.
4. Different cloudiness levels and types of clouds.

That makes for GeographyDev complete. Next comes the SurvivalDev 0.2.
