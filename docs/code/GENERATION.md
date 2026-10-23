# Native Terrain-v2 generation

`mc_generation` owns the deterministic CPU atlas kernels and configuration;
`mc_world_sampling` adds canonical regions, surface columns and chunks. These
targets require no window, renderer, network startup or CoreLang initialization.
The standalone `mc_world_map` exporter uses the same atlas and sampler as gameplay.
Diagnostic RGB interpolation uses fused multiply-add for stable rounding.

The atlas pipeline publishes immutable stage snapshots from tectonics and
coasts through drainage, climate and erosion to the final fields. Progress
snapshots describe completed real work; consumers may coalesce snapshots but
cannot invent a completed atlas. Cancellation and failures remain explicit.
Configuration validation precedes allocation and generation.

Canonical regional elaboration inherits its parent samples. `TerrainWorld`
provides typed surface columns and materializes actual chunks from those columns;
see [GAMEPLAY.md](GAMEPLAY.md) for authority, replication, coverage and edits.
The bottom layer is Bedrock. Land columns use a top material, four ground
layers, then Stone. Below sea level, the ground band is limited to the four
blocks immediately below sea level. Climate selects the materials at source
precision; rendering and collision consume the same sampled world.

`WorldPreparation` executes the bounded CoreLang generation plan before atlas
production. Live scheduling gives each regional job its exact immutable parent;
workers refine that parent or sample the installed 64-block leaf directly.
 Shared leases coalesce requests and reject stale identity/epoch results. Retained/reserved regional data and completed results have separate
32 MiB budgets. Accepted pipelines stay capped at 192; retained-chain admission
queues excess work and retries failures. Waiting job-zero pipelines cancel safely.
CPU LOD climate keeps double precision through interpolation/classification.
Prepared joins queue height and materialization work before
publishing a resident-only spawn; server ticks do not generate terrain.

Numerics and fixture provenance belong to the
[versioned numerical contract](../ai/decisions/earlydev_0_1_1_s1_numerics.md).
Revision-3 references remain frozen. Revision 4 changes shelf exponentials to
binary64 followed by binary32 conversion, requiring the exact
`shelf-exp=exp64-to-f32-v1` configuration token. Model identities are incompatible; other numerical constraints remain.
Revision 5 retains those atlas kernels and five regional stages, then adds
16-block seeded relief to final columns after the 64-block leaf. Regional samples stay unchanged; previews/materialization share the local kernel.
Its manifest binds samples; revision-4 fixtures remain.
Six-seed agreement does not establish universal host-libm equivalence.

Revision 6 changes only the exposed-land top-material choice: land warmer than
−2 °C uses Grass regardless of precipitation, while cold land keeps Snow.
Ground/height/atlas remain unchanged. `TerrainWorld::sampleClimate`
returns canonical periodic bilinear temperature/precipitation for finite XY.
LOD nodes use it; arbitrary LOD samples interpolate scalars and reclassify tops. The revision-5 dry-land Dirt choice remains in its frozen reference
and cannot be reused as revision-6 identity or cache data. Fixtures bind seeds and rays.

Fixtures bind samples/hashes. Reference tools compare outputs without approving changed expectations. Follow [WORLD_MAP.md](../ai/WORLD_MAP.md)
for the exporter and [VALIDATION.md](../ai/VALIDATION.md) for focused and full
verification selection. Platform acceptance stays separate.

## Layered terrain LOD model

LOD uses the separate visual identity `lod-canonical-r6-height-v2`, not
Terrain-v2 revision 6; frozen R6 fixtures remain unchanged. L0 spans the full
65,536-block periodic world at 256 or 512 unique samples per axis plus seam
edges. L1 and L2 each refine cells 4×4. LOD metadata carries water height and
presence, but the scene renders no 3D water. L3 samples canonical columns. Linear triangle fans share constrained edge
chains across equal/mixed levels and periodic seams. Complete actual capture
or conservative closure replaces one 16-block L2 cell at 256 resolution, or
four 8-block cells at 512; lower-volume gaps retain fallback. This changes
presentation, not Terrain-v2 identity. LOD nodes use canonical R6
`sampleSurfaceHeight`; actual generation is unchanged.

`TerrainLodSource` asynchronously builds a separate client atlas from the exact
session configuration, cancels superseded work and publishes only matching
identity/epoch results. This duplicates server atlas work; server chunks, collision, edits and state
remain authoritative. `TerrainLodRuntime` shares desktop/Android capture and
publication through persistent deadline-bounded discovery/capture queues; movement
retains useful columns. Scene builds validate exact captured sources.

## Bounded materialized prefetch

Authoritative readiness is horizontal 40 chunks = 640 blocks: 81×81 XY profiles,
each describing all 64 vertical chunks. Protected interest is 768 blocks,
97×97 profiles per client (9,409), at most 37,636 across four clients. Profiles
encode exact highest-solid/layer/material data and dense overrides; they are
not height-only previews. Profile eviction removes readiness. An ordered eligible index retains the same
oldest-unprotected victim order, pins and cap without full-cache scans.

Dense collision/start materialization separately covers 48 blocks with a 32-block
lead (80 interest), bounded to 1,024 cached chunks per client. Server start waits
for dense near ACKs; full 640 readiness cannot be inferred from that cache.
Visual 320 metadata is separate; L0 spans the periodic world. Resident dense lead
chunks promote without retransmission. Same-epoch connectivity retains confirmed
overlap or reroots. The historical 40/72-block model was a unit error; its
receipts cannot qualify the corrected full-volume contract. Measure total memory
and actual lead sufficiency separately.


## Shared surface sampling

The worker pool reuses completed immutable 16×16 surface tiles across preview
and materialization jobs at different Z. Its bounded LRU holds at most 8,464
tiles: four projected 352-block interests, conservatively 46×46 XY chunk
coordinates each. Payload is approximately 83 MiB, excluding key/index and
allocator overhead; measure total live memory separately.

Keys bind terrain identity, epoch, regional leaf and XY. Validation precedes
lookup; sampling runs outside the mutex, and concurrent misses may duplicate
work. Cancellation precedes lookup/insertion. Canonical authoritative identity
ignores unchanged dense-cache churn but includes genuine edits, deletion and
profile replacement. Cached samples alone never establish Ready.
