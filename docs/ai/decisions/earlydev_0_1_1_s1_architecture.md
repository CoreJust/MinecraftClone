# S1 — Generation ownership and scheduling

This future production design replaces the fixed-wave generator. The
[experiment](earlydev_0_1_1_s1_experiments.md) qualifies the model, not native
runtime behavior. Source ownership was inspected at
`2e3e4e2adefa51521ab513f5a5bacd87a59be21a`.

## One world context

`GameServer` owns one `GenerationSession`: validated configuration, immutable
atlas, artifact store, coordinator and worker cancellation scope. `SparseWorld`
shares its context. Workers receive immutable handles; the server alone
publishes results. There must be no separate default generator in synchronous
collision/query paths.

`GenerationConfiguration` contains schema/algorithm version, seed, dimensions,
atlas resolution, regional levels/sample counts, iteration bounds, all model
coefficients/units, vertical mapping, material rules and CoreLang source/manifest
digest. Canonical serialization fixes field order, integer widths and numeric
encoding, rejects nonfinite/out-of-range values and hashes complete content.
Operational queue/cache budgets are excluded because changing them cannot
change terrain. The existing stone-fixture `chunk_content_digest` is not this
configuration identity.

`WorldIdentity` is the canonical configuration plus digest; equality verifies
content as well as digest. `SessionEpoch` identifies one activation.
`ArtifactKey` adds kind, canonical wrapped origin, level/extent, and Z for
chunks. Jobs/results carry key, epoch and attempt ID. Seed, material, script
or parameter changes invalidate dependent artifacts even if the algorithm
version was accidentally unchanged. Randomness derives from seed/domain/stable
feature IDs, never worker order.

## Typed immutable artifacts

| Contract | Inputs and output |
|---|---|
| `prepareAtlas(config,cancel,progress)` | Private global build; validate and publish one `GlobalAtlas`, or typed failure. |
| `GlobalAtlas` | Bed/spill, plates/crust/relief, connected water IDs, receivers/discharge, climate, coast regime/fragment/arc/hotspot constraints and provenance. |
| `refineRegion(key,atlas,exactParent,cancel)` | One typed `RegionalArtifact`: constrained relief, climate/water/material samples, exact parent identity and boundaries. |
| `sampleSurface(context,leaf,tileKey)` | One 16² `SurfaceTile`: highest solid H, water presence/height, top/ground materials and layer intervals. |
| `materializeChunk(context,tile,chunkKey)` | Existing 16³ `Chunk` storage filled from those same intervals. |

Highest-solid H is inclusive. Water is diagnostic in S1; Air remains above H.
Do not copy the full atlas into regional jobs, opaque byte payloads or preview
messages. Immutable reference-counted handles lease inputs until jobs finish.
CoreLang remains the bounded parameter/plan declaration entry point; native
kernels execute the measured model. Invalid plans fail visibly without a
silent cosine-terrain fallback.

## Dependency lifecycle

The global DAG is tectonics → coastal elaboration → initial drainage → provisional climate → terrain
shaping/rerouting → final climate → final drainage → validation/publication.
Atlas readiness gates world-dependent spawning and collision. Then resolve
the existing regional hierarchy `16384→4096→1024→256→64`, canonical surface
sampling and materialization. A missing exact parent is a dependency.
Provisional visuals may use an explicitly tagged ancestor quality level;
authoritative chunks require the prescribed leaf.

States are WaitingForDependency, Queued, Running, Ready, Failed and Cancelled.
Expose phase and completed/total units for measurable loops. Check cancellation
between epochs, flood batches, climate iterations and regional tiles. Build
privately; never publish a partly filled atlas.

Deduplicate artifacts with multiple request subscribers. Shared dependencies
inherit their most urgent subscriber's distance/view/movement priority; reserve
dispatch for collision-critical work and age background jobs. Removing one
subscriber cannot cancel other consumers. Cancel on zero consumers or epoch
replacement. Accept completion only if key, epoch, attempt and subscribers
still match. Failed preparation yields a recoverable world-creation error,
not gameplay with mismatched collision.

## Capacity and reproducibility

Separate pinned-atlas, build-scratch, regional LRU, in-flight leases/results,
preview and resident-chunk accounting. Reserve scratch/result bytes before
dispatch and reconcile actual allocations. Eviction cannot remove leased
parents. Capacity pressure postpones work or returns a typed capacity failure;
it cannot silently change resolution. Existing 4 KiB stage outputs and 1 MiB
refinement retention are inadequate for these artifacts. Default capacities
and measured live sets are specified in the experiment report; qualify actual
native/Android values before release.

Use explicit fixed-width arithmetic for procedural hash addresses and stable
row-major/tie ordering. The native port must define canonical PRNG/math and
quantization, rather than inheriting `std::hash`, library random distributions
or unchecked transcendental behavior. Its concrete numerical contract and
cross-platform gates are in the [implementation sequence](earlydev_0_1_1_s1_implementation.md).
The Python experiment establishes same-environment repeatability only.
