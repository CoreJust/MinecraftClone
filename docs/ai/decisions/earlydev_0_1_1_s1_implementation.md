# S1 — Production implementation work packages

Ready work packages derived from the [architecture](earlydev_0_1_1_s1_architecture.md)
and qualified [model](earlydev_0_1_1_s1_generation.md). They are future work,
not completed game tasks. Create one executable basic task per bounded slice;
start inherited fixes immediately alongside independent feature lanes. Use the
[parallel major plan](earlydev_0_1_1_s1_iterations.md) for scheduling; package
numbers identify scope, not a serial execution order. Retain ordinary-gameplay
evidence throughout.

## Work packages and dependency joins

1. **Inherited product defects.** MC-AI-0477/0478/0479 own cursor capture,
   fresh moving frames and near-first visible streaming. Preserve actual-path
   regressions and recheck them on final S1 geography.
2. **CoreLang dependency/backend adapter.** Obtain the exact temporary 0.1.5
   ai-dev handoff; update lock, clean package provenance and affected consumers.
   Integrate actual JIT/AOT APIs, explicit backend reporting and interpreter
   parity for generation plans, scenarios and policy. Follow the
   [pin/fix/release contract](earlydev_0_1_1_s1_corelang.md).
3. **Module separation.** Implement the [source-reviewed boundaries](earlydev_0_1_1_s1_modules.md):
   CPU `mc_generation`, extracted worker/scheduler service, CoreLang adapter,
   authoritative gameplay/transport and selectable presentation. Build standalone
   numerical/service/2D consumers without Vulkan; preserve normal compositions.
4. **Identity and reference numerics.** Extend `World.hpp/.cpp`,
   `WorldGeneration.hpp/.cpp` and plan validation with complete immutable
   configuration, typed artifacts, schema/version and content digest. Specify
   retained PCG64 global streams, local mixer, rounding, units and nonlinear
   math explicitly; do not substitute a new random sequence. Freeze raw Python
   height/material/color fixtures and require exact reference pixels plus native
   cross-platform digests. Keep the world stable for version/seed/config;
   numerical incompatibility requires an explicit versioned resolution, not
   automatic corpus regeneration. See [testing](earlydev_0_1_1_s1_testing.md).
5. **Global kernels and map export/progression.** Port the selected model with
   fixed bounds, cancellation and memory reservations; retain causal variants.
   Emit immutable diagnostic map snapshots during work. The CPU map exporter
   and server share configuration, atlas and sampling without GPU initialization.
6. **Regional DAG, readiness and frontier.** In `WorldGenerationScheduler.hpp/.cpp`,
   replace opaque bytes with typed parent handles; update request/dispatch,
   admission, leases, stale/cancel/dedup and eviction. Inject real dependencies,
   classify uniform chunks exactly, enumerate Ready spheres and publish a
   player-rooted connected frontier. Update bounded CoreLang stage declarations
   with host manifests. Benchmark the required sphere and x30 prefetch lead.
7. **Canonical blocks, materials and wire.** Replace `GameServer.cpp`'s opaque
   `generateRegionOutput`; pass actual leaf/surface artifacts through
   `HeightTileWorkerPool::executeGenerationJob` into materialization. Inject
   that context into `SparseWorld::ensureChunk`/`blockAt`; remove default-wave
   bypasses. Test exact layers and preview/chunk/collision parity. Add palette
   IDs and update `Message.hpp/.cpp`, both codecs and protocol identity with
   bounds/credit validation; include quality/world identity in mesh keys.
8. **Loading gate and 2D normal-game mode.** Add actual progressive map,
   refined safe spawn, 48-block Ready sphere and any-key gate before movement
   or cursor capture. The column-color adapter reads the same published chunks
   into bounded in-memory texture tiles. Reuse production scheduling, simulation,
   networking and trace inputs; compare all raw pixels/coverage.
9. **Normal renderer and LOD.** Update `GameClient` ingestion, `PreviewMeshing.hpp`,
   `HeightTileSurfaceMesher` and stone-specific draw paths. Merge only compatible
   materials; expose correct top/ground/Stone cliffs and retain seams/LOD.
   Full voxel edit meshing remains S2; physical water remains S3.
10. **Actual delivery qualification.** Execute the subsystem matrix, six-seed
    exact references, order/movement/teleport/connectivity replays and offscreen
    views as features integrate. Measure normal Mac/Windows loading and x30
    radius/FPS concurrently from the first runnable slice; reserve the full
    large matrix for the frozen candidate. Qualify
    both two-laptop host roles, applicable Android gates and exact-package owner
    playtests. Use the [performance](earlydev_0_1_1_s1_performance.md) and
    [multiplayer](earlydev_0_1_1_s1_multiplayer.md) contracts. Start implementation's
    planned 4h/12h lifecycle; no Python/2D pass substitutes for normal GPU results.

## Standalone map command

Add a dedicated `mc_world_map --seed N --atlas-size 256 --layers ... --output DIR`,
plus `--seed-list 0,1,42,777,2004,8800555` and a detailed regional extent option.
Link only `mc_generation` and its CPU dependencies, allowing the tool alongside
normal client builds. A `mc_main` subcommand would still link graphics: the
current executable depends on monolithic `mc`, whose client branch links
Vulkan/GLFW. Merely skipping window initialization would not satisfy this target.

Use the exact same config/atlas/sampler as gameplay. Add a small size-checked
production PPM writer plus JSON configuration/timing/memory/check metadata and
binary field layers. The existing `writePpm` is in test support
(`tests/testsupport/ImageComparison.cpp`), not a reusable production
`RenderCapture` writer. An optional Python wrapper can encode PNG; no graphics
or PNG dependency belongs in the CPU target. Separate simulation, queries,
rasterization and serialization costs; preserve ordered seeds and rejected runs.

When contracts actually change, reconcile `docs/code/GAMEPLAY.md`,
`docs/scripting/README.md` and the code index against source before refreshing
their hashes. This planning task does not falsely describe the design as
existing production behavior.
