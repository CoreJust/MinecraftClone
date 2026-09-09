# AI delivery roadmap

This is an implementation queue, not the product specification. [../ROADMAP.md](../ROADMAP.md) remains canonical for product intent; [../VERSION_CONVENTION.md](../VERSION_CONVENTION.md) defines releases and snapshots.

## Operating contract

- Use one task file per stable ID, based on [tasks/TEMPLATE.md](tasks/TEMPLATE.md). Keep a requirement-to-test mapping in that file.
- Treat the repository-declared `EarlyDev 0.1.0:3` as an **unverified baseline**: inspect the current diff and run its gates before claiming any prior snapshot works. Never fold unrelated dirty changes into a task.
- Each task owns client, server, shared protocol, content, assets, persistence, and tests only when its scope needs them. Server authority, deterministic generation, bounded resource use, and versioned save/network formats are default invariants.
- Every completion records exact commands and results. Common gate: `cmake --preset debug`, `cmake --build --preset debug`, `ctest --preset debug --output-on-failure`, and `python3 publish.py EarlyDev:Initiation 0.1.0:3 --checks-only` (substitute the delivered version). Add deterministic unit/integration tests and a two-process runtime smoke when behavior crosses the network or renderer boundary.
- IDs are permanent even if tasks split: `ED-010-Sn`, `ED-01n`, and `GD-02n`. `D:` denotes a decision that must be recorded before implementation, never silently guessed.

## Near horizon — Initiation snapshots

| ID | Depends on | Deliverable and observable acceptance | Validation |
|---|---|---|---|
| ED-010-S1 | — | Content-free runnable application (no server/client, renderer, or world yet); project folders/docs, CMake+vcpkg build, `spdlog`/`fmt`, and core macros/assertions/crash handling; it logs startup. | configure/build; startup smoke; core unit tests. |
| ED-010-S2 | S1 | Basic network API and separated server/client; real players and bots with 2×2 footprints roam a console-rendered 32×32 flat 2D world, with server collision. | codec/world collision tests; server plus two clients smoke. |
| ED-010-S3 | S2 | Core Vulkan infrastructure, GLFW window, Vulkan+GLFW world rendering, and publish-check script; the S2 scene visibly renders. | validation-layer renderer smoke; fallback-capable GPU smoke; common gate. |
| ED-010-S4 | S3 | 3D visual scene over still-flat logic; real or bot players; real-player camera and WASD; grid floor and colored box players. | input/camera math tests; bot and interactive movement smoke. |
| ED-010-S5 | S4 | One chunk made only of air and dirt; flying player can inspect its block geometry. | chunk/meshing tests; fly-through renderer smoke. |
| ED-010-S6 | S5 | Sparse/chunked 65,536×65,536×1,024 addressable world with deterministic air/stone pattern; no full-world allocation. | boundary/streaming/memory tests; distant chunk smoke. |
| ED-010-S7 | S6 | X/Y world wrapping across simulation, protocol, and rendering. D: profile first, then record justified render/network optimizations and budgets. | seam/collision/replication tests; wrap-crossing smoke; profile artifact. |
| ED-010-S8 | S7 | Permission-gated flight and phasing through blocks/entities; without permission, jump, gravity, block collision, and entity collision work. | permission and physics edge-case tests; multiplayer smoke. |

`ED-010-S1`–`S3` are historical snapshot descriptions; use the same acceptance checks to establish their actual state before extending them. `S4`–`S8` are the remaining planned snapshots.

## EarlyDev minor releases

| ID | Depends on | Scope and acceptance | Validation |
|---|---|---|---|
| ED-011 BasicBlocks | ED-010-S8 | Grass, stone, bedrock, snow, ice, water; fixed-pattern worldgen; permission-aware place/break; correct first/second/third-person player models/views. | block-state, edit-authority, serialization tests; all-view multiplayer smoke. |
| ED-012 GUI | ED-011 | Main screen; create/join-world screen; pause; hotbar and inventories; save/open world; F3 diagnostics. Save/load preserves world and inventory. | UI state/save compatibility tests; create, join, pause, reload smoke. |
| ED-013 BasicWorldGen | ED-012 | Global height/temperature continents and oceans; local deep-ocean/ocean/beach/plains/hills/mountains biomes; sand; icy poles and extreme north/south ice wall; trees. | fixed-seed map/biome boundary tests; map preview smoke. |
| ED-014 SkyGraphics | ED-013 | Lighting/effects, day/night, sun/moon/stars/clouds, and latitude-dependent sky position/block tint. D: choose lighting model and performance target. | deterministic time/latitude tests; day-cycle visual smoke. |
| ED-015 Crafting | ED-014 | Inventory UI; recipes; workbench/chest UIs; planks, sticks, wooden shovel/axe/pickaxe; tool requirements/different drops including cobblestone; stone tools. | recipe, container, tool/drop and authority tests; crafting smoke. |
| ED-016 Chat | ED-015 | Nicknames, replicated chat, permissioned Turing-complete commands, and test command blocks. D: define command sandbox, quotas, persistence, and command-block access before coding. | parser/authorization/resource-limit tests; two-client command smoke. |
| ED-017 Ores | ED-016 | Coal/copper/tin/iron ores; furnace and melting; copper/tin/bronze/iron ingots; metal blocks/tools; ore and cave generation; torches. | seeded generation, recipe, light-source tests; underground smoke. |
| ED-018 Mobs | ED-017 | Health bar, combat system, and peaceful mobs with server-authoritative state. | damage/targeting/replication tests; combat and passive-mob smoke. |
| ED-019 Enemies | ED-018 | Dead miners, zombies, bats and other hostile mobs; player death/respawn; wooden/metal armor; swords and other weapons. D: enumerate the initial weapon set and balance values. | spawn/combat/death/respawn/armor tests; hostile encounter smoke. |

**EarlyDev exit:** complete cross-version save/network migration checks, performance/error regression pass, and release executable evidence; record any deferred improvements instead of declaring them complete.

## GeographyDev minor releases

| ID | Depends on | Scope and acceptance | Validation |
|---|---|---|---|
| GD-020 Map | ED-019 | Seas, rivers, lakes, ponds, islands, minimap, world map, and gravel if selected. D: decide the original “gravel, ?” scope and water-map representation. | fixed-seed hydrology/topology tests; map navigation smoke. |
| GD-021 SurfaceBiomes | GD-020 | Deserts with sandstone/cactus; tundra with frozen dirt; taiga, “step” (D: clarify whether this means steppe), savanna, jungle; better terrain. | biome-material and transition tests; seed gallery smoke. |
| GD-022 Forests | GD-021 | Realistic structures/placement, meadows, young/old/bush/fallen/dead trees, and pine, spruce, oak, birch, weeping willow, palm, acacia, maple, ginkgo, sakura, baobab, giant sequoia, banyan, tree-like cactus, olive, juniper. | species grammar/placement tests; biome traversal smoke. |
| GD-023 Caves | GD-022 | D: define the tentative layered/rifted cave kinds; surface, dirt+some-stone underground, stone caves, deep caves with compressed-stone decision and occasional lava, and nether with netherstone/lava bodies; layer-specific specialized ores and additional minerals. | layer/ore/lava safety tests; vertical exploration smoke. |
| GD-024 Underwater | GD-023 | Weeds of all kinds, complex ocean terrain/new blocks, and currents affecting entities. | current physics/terrain generation tests; underwater traversal smoke. |
| GD-025 SkyWeather | GD-024 | Sky islands, seasons, rain, hail, wind, snow, lightning, and cloudiness levels/types. | seeded weather/time transitions and damage/physics tests; season/weather visual smoke. |

**GeographyDev exit:** deterministic world-generation regression corpus, world/map save compatibility, multiplayer weather/world sync, and release executable evidence. The next named stage is SurvivalDev; do not invent its scope here.

## Sequencing rules for an AI implementer

1. Open the target task and its direct dependencies; verify the current baseline before edits.
2. Make schema/protocol/save changes first, then server authority, shared simulation, client rendering/UI, and content/assets.
3. Add meaningful tests with the behavior; run focused checks while iterating, then the common gate and the task's observable smoke.
4. Update the target task with results, unresolved decisions, migration notes, and a concise [../ROADMAP.md](../ROADMAP.md) alignment note. A release needs a version-history entry after its evidence exists.
