# S1 — Production implementation sequence

Ready work packages derived from the [architecture](earlydev_0_1_1_s1_architecture.md)
and qualified [model](earlydev_0_1_1_s1_generation.md). They are future work,
not completed game tasks. Create one executable basic task per bounded slice;
keep inherited fixes first and ordinary-gameplay evidence throughout.

## Ordered slices

1. **Fix the inherited product defects.** MC-AI-0477/0478/0479 own cursor,
   moving fresh frames and near-first visible streaming. Preserve actual-path
   regressions and rerun them on final S1 geography.
2. **Identity and numerical contract.** Extend `World.hpp/.cpp`,
   `WorldGeneration.hpp/.cpp` and CoreLang plan validation with immutable
   configuration/context, full keys and typed artifacts. Canonical config uses
   little-endian fixed-width fields and fixed-point coefficients, with schema
   version and content digest. Use the retained explicit SplitMix64-style
   mixer for domain/feature random streams and local lattice values; no library
   distribution defines the save/world format. Native authoritative kernels
   use specified rounding, stable reductions and versioned lookup tables for
   nonlinear functions, not platform `libm` behavior. Quantize published
   geological heights to millimetres, climate to 0.01 °C and model flux to
   1/65536 before shared sampling. Pin lookup tables/reference vectors in
   source. This is a new native algorithm revision: NumPy's exploratory PCG64
   sequence is not a native golden. Regenerate/reinspect the corpus, and require
   exact native field/material digests on Mac/Android and Windows build tests
   where available; keep owner Windows runtime restriction separate.
3. **Global kernels and headless atlas export.** Port geology, drainage,
   bounded terrain shaping and climate with fixed pass counts, cancellation
   and memory reservations. Test zero-motion/current/wind causal variants,
   connected sea, lake outlets and DAGs. Compare native morphology/statistics
   and costs with the experiment. Add a CPU-only map path before any window or
   GPU initialization; it uses the same factory as the server. Extract a CPU-only
   `mc_generation` library from the monolithic `mc` target for atlas/configuration,
   shared world sampling and map encoding; both gameplay and `mc_world_map` link it.
   Keep scheduler/client/network ownership outside this numerical library.
4. **Regional DAG and cache.** In `WorldGenerationScheduler.hpp/.cpp`, replace
   opaque refinement bytes with typed immutable handles. Update `requestPlan`,
   dispatch, exact-parent lookup, acceptance and invalidation. Test shared-job
   deduplication, priority inheritance, cancellation, byte admission, eviction,
   stale epochs and request-order independence. Update CoreLang host manifests
   and bounded stage declarations together.
5. **One authoritative sampler.** Replace `generateRegionOutput`'s 16² opaque
   height buffer in `GameServer.cpp`; pass actual leaf/surface input to
   `HeightTileWorkerPool::executeGenerationJob`. Materialize currently calls
   the overload that ignores inherited refinement. Inject the same context
   into `SparseWorld::ensureChunk` and mutable `blockAt`, which currently use
   their own default generator. Test preview/chunk/collision parity before
   accepting these paths. Choose a bounded deterministic safe land spawn with
   standing clearance after atlas readiness.
6. **Materials and wire.** Add stable palette IDs; update `Message.hpp/.cpp`,
   descriptor/join identity and both codecs together with a protocol bump.
   Surface payloads carry H and top/ground material/interval metadata, not the
   atlas. Validate IDs/heights/layers and actual encoded-size credits before
   mutation. Include world identity and quality in residency/mesh keys. No
   unrequested old-protocol adapter is required.
7. **Normal renderer and LOD.** Update `GameClient` preview ingestion,
   `PreviewMeshing.hpp`, `HeightTileSurfaceMesher` and stone-specific
   draw/instance paths. Quads/merges require matching material; cliffs expose
   correct top, four ground layers and Stone. Preserve seams, shoreline and
   material transitions during LOD. Snow/Ice are solid S1 materials. Water
   transparency/rendering and physical water remain S4; map colours do not
   implement either. Full voxel edit meshing remains S2.
8. **Qualify the actual delivery.** Extend the existing generation/scheduler,
   sparse-world, server-preview, message and player-LOD suites. Add fixed-seed
   native map checks, offscreen material/cutaway visuals, malformed payload and
   stale-result tests. Then normal-launcher Mac/Android play, moving-frame and
   full-viewport streaming acceptance, aggregate/release gates and exact-package
   owner acceptance or scoped exception. Follow the later implementation's
   mandatory PDF-report lifecycle. No prototype result closes these gates.

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
their hashes. This planning task does not falsely describe the proposal as
existing production behavior.
