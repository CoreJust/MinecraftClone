# Native Terrain-v2 generation

`mc_generation` owns deterministic CPU atlas kernels/configuration;
`mc_world_sampling` adds canonical regions, columns and chunks. Neither needs window, renderer, network or CoreLang startup. `mc_world_map`
exports with gameplay's atlas/sampler. RGB diagnostics use fused multiply-add
for stable rounding.

With a callback, the atlas publishes immutable snapshots after tectonic, coast,
drainage, climate and erosion. Without one, snapshot work is skipped; final fields match. Consumers may coalesce snapshots; readiness requires
a completed atlas. Cancellation/failure stay explicit; configuration validates
before allocation.

Canonical regional elaboration inherits parent samples. `TerrainWorld`
provides typed columns and builds actual chunks; [GAMEPLAY.md](GAMEPLAY.md)
covers authority, replication, coverage and edits. Columns begin with Bedrock,
then top material, four ground layers and Stone; below sea level, ground uses
the four blocks immediately beneath it. Climate selects materials at source
precision; rendering and collision use those samples.

`WorldPreparation` runs the bounded CoreLang plan before atlas production.
Regional jobs refine their exact immutable parent or sample the installed 64-block leaf. Shared leases coalesce requests and reject stale
identity/epoch. Retained/reserved regional data and completed results have
separate 32 MiB budgets; pipelines cap at 192. Excess chains queue and retry;
waiting job-zero pipelines cancel safely. CPU LOD keeps double precision through
climate interpolation/classification. Prepared joins queue height/materialization
before resident-only spawn publication; server ticks never generate terrain.

## Nearest-land prepared spawn

Without a configured spawn, prepared Flight selects the nearest dry integer
origin to `(32768,32768)`, ordered by shortest wrapped XY distance, squared
distance, Y, then X. Require dry `highest_solid_z + 1` (clear `water_present`/`submerged`), bounds, materialized support, body clearance and no overlap.

Search periodic rectangles by minimum toroidal distance; certify only when
every unvisited lower bound exceeds the candidate. Polls handle at most 128
frontier/candidate items. Height and collision stay asynchronous; polling never
generates terrain. Exhaustion fails without fallback.

`hasWaterPresenceThroughoutAtlasCell` prunes only complete aligned 256/512 R6
cells with ocean/lake/river masks. R6 selects mask pixel
`floor(coordinate * resolution / 65536)` and carries its categorical flag
through refinements. Each cell covers `[origin, origin + extent - 1]`, including
the final pre-seam column. False, mixed or clipped cells require exact refinement. Future versions must prove all columns wet.

Numerics/provenance: [versioned contract](../ai/decisions/earlydev_0_1_1_s1_numerics.md).
R3 references stay frozen. R4 uses binary64 shelf exponentials then binary32,
requiring `shelf-exp=exp64-to-f32-v1`; model identities are incompatible and
other constraints persist. R5 keeps atlas kernels/five regional stages,
then adds seeded 16-block relief after 64-block leaf. Regional samples stay
unchanged; previews/materialization share the local kernel. Its manifest binds
samples; R4 fixtures stay frozen. Six-seed agreement does not prove universal
host-libm equivalence.

R6 changes exposed-land tops only: Grass above −2 °C regardless of
precipitation, Snow otherwise. Ground, height and atlas stay unchanged.
`TerrainWorld::sampleClimate` returns canonical periodic bilinear
temperature/precipitation for finite XY; LOD nodes use it, while arbitrary
samples interpolate then reclassify. R5 dry-land Dirt fixtures stay frozen and
cannot identify R6 caches. Fixtures bind seeds and rays.

Fixtures bind hashes; comparison tools never approve changed expectations. See [WORLD_MAP.md](../ai/WORLD_MAP.md) for the exporter and
[VALIDATION.md](../ai/VALIDATION.md) for verification selection. Platform
acceptance stays separate.

## Layered terrain LOD model

LOD uses visual identity `lod-canonical-r6-height-v2`, distinct from Terrain-v2
R6. L0 spans the 65,536-block periodic world at 256/512 unique samples per axis
plus seam edges; L1/L2 refine 4×4. Metadata carries water height/presence, but no
3D water renders. L3 samples canonical columns. Triangle fans share constrained
edges across levels and periodic seams. Complete captures or conservative
closure replace one 16-block L2 cell at 256, or four 8-block cells at 512;
gaps retain fallback. Presentation uses canonical R6 `sampleSurfaceHeight`; generation is unchanged.

`TerrainLodSource` asynchronously builds a separate client atlas from the exact
session configuration, cancels superseded work and publishes matching
identity/epoch only. It duplicates server atlas work; server chunks, collision,
edits and state remain authoritative. `TerrainLodRuntime` shares desktop/Android capture/publication through
persistent deadline-bounded discovery/capture queues; movement retains useful
columns. Scene builds validate exact captured sources.

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

The worker pool reuses immutable completed 16×16 tiles across preview and
materialization at different Z. Its LRU holds 8,464 tiles: four projected
352-block interests at 46×46 XY coordinates each. Payload is about 83 MiB,
excluding index/allocator overhead; measure total live memory separately.

Keys bind terrain identity, epoch, regional leaf and XY. Validation precedes lookup; unlocked sampling permits duplicate concurrent misses. Cancellation precedes lookup/insertion. Canonical authoritative identity
ignores unchanged dense-cache churn but includes genuine edits, deletion and
profile replacement. Cached samples alone never establish Ready.
