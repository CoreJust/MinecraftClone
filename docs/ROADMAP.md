# Roadmap to the next versions (estimated, up to changes and elaboration)

Next version is EarlyDev 0.1.0 - Initiation.

Planned path to it (per snapshot):

1. The first runnable version of the game (without any content at all).
2. Network test and early project structure outline. The game can run a server and several clients, client have players 2x2 in a 32x32 flat world and generate random movements, the server handles the collision. Rendering is implemented via console.
3. Rendering test. The logic stays the same, but now it is rendered within a window with real Vulkan graphics.
4. 3D and controls test. The world becomes 3D (although flat in logic), the players can be real or bots, the real ones have a working camera and WASD movement. The world has a grid below and simple colored box players.
5. Blocks test. The world consists of a single chunk with only 2 block types: air and dirt. The player flies around it.
6. World test. The world is full 65'536 x 65'536 x 1'024 blocks. The chunks are still only air and stone with a simple generation pattern.
7. Wrap test. The world becomes wrapped in X and Y directions, probably some rendering and network optimizations here.
8. Player permissions and physics test. Now players require permissions to pass through blocks/other entities and to fly. Without those, physics are enabled - players can jump, do not fall through blocks and each other.

Plans for EarlyDev 0.1.1 (not split into snapshots) - BasicBlocks:
1. A basic set of blocks (grass, stone, bedrock, snow, ice, water).
2. Simple worldgen (probably still not random, but with fixed patterns).
3. Blocks placing and breaking (and corresponding permissions).
4. Proper player models, first/second/third person viewes.

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
3. Minimap and world map.

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
