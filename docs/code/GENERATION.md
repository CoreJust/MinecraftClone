# Native Terrain-v2 generation

`mc_generation` owns deterministic CPU atlas kernels and configuration;
`mc_world_sampling` adds canonical regions, columns and chunks. Both are free of
window, renderer, network and CoreLang startup dependencies. `mc_world_map`
exports gameplay's atlas/sampler. RGB diagnostics use fused multiply-add for
stable rounding.

With a callback, the R6 atlas publishes immutable snapshots after tectonic,
coast, drainage, climate and erosion. Without one, snapshot work is skipped;
final fields match. Cancellation and failure stay explicit, configuration is
validated before allocation, and readiness requires a completed atlas.

Canonical regional elaboration inherits parent samples. `TerrainWorld` provides
typed columns and builds actual chunks; [GAMEPLAY.md](GAMEPLAY.md) covers
authority, replication, coverage and edits. Columns begin with Bedrock, then top
material, four ground layers and Stone; below sea level, ground uses the four
blocks immediately beneath it. R6 selects Grass above −2 °C and Snow otherwise;
atlas height and ground stay unchanged. Climate queries return periodic
bilinear temperature and precipitation.

Numerics and provenance: [versioned contract](../ai/decisions/earlydev_0_1_1_s1_numerics.md).
R3 references stay frozen. R4 uses binary64 shelf exponentials then binary32,
requiring `shelf-exp=exp64-to-f32-v1`. R5 retains atlas kernels and five regional
stages, then adds seeded 16-block relief after the 64-block leaf. Regional
samples stay unchanged; previews and materialization share the local kernel.
Fixtures bind seeds and hashes; comparison tools do not approve changed
expectations. Six-seed agreement does not prove universal host-libm equivalence.

## Nearest-land prepared spawn

Without a configured spawn, prepared Flight chooses the nearest dry integer
origin to `(32768,32768)`, ordered by wrapped distance, squared distance, Y,
then X. Require dry `highest_solid_z + 1`, valid bounds, materialized support,
body clearance and no overlap. Search periodic rectangles by minimum toroidal
distance and certify only when unvisited lower bounds exceed the candidate.
Poll at most 128 frontier/candidate items; height and collision stay
asynchronous, and exhaustion fails without fallback.

`hasWaterPresenceThroughoutAtlasCell` prunes only complete aligned 256/512 R6
cells. R6 selects mask pixel `floor(coordinate * resolution / 65536)` and
carries its ocean/lake/river flag through refinements. Cells include the final
pre-seam column; false, mixed or clipped cells require exact refinement.

## Layered terrain LOD model

R6 visual identity `lod-canonical-r6-height-v2` differs from Terrain-v2 R6.
L0 spans the periodic world; L1/L2 refine 4×4 and L3 samples canonical columns.
Triangle fans share constrained edges and seams. Complete captures replace one
16-block L2 cell at 256 or four 8-block cells at 512; gaps retain fallback.

V9 runs the frozen native Platec, terrain, climate and water stages. Terrain
retains source metres; climate fields retain physical units. Structural
hierarchy queries return source-metre H0–H3 values, with L0 coarse and L3
finest. World overview may use L0, while canonical surface, collision and edit
queries use L3. The game mapping is globally fixed as
`floor(384 + source_m / 10)` over blocks 0–1023; out-of-domain queries fail,
and the model never normalizes each seed independently. Atlas registries retain
source fields and dtypes. Columns remain solid layers followed by air; no caves
or overhangs are generated. Once the structural memo reaches its logical cache
budget, later misses are recomputed into caller-owned values; saturation does
not invalidate a valid world query. Snow/Sand material use follows pipeline
integration. Broader numeric bounds, full L0–L3/seam coverage, cross-platform
parity and generator performance remain aggregate acceptance gates.

## Native V9 seasonal climate

The private climate component consumes the canonical 256×256 terrain bed and
runs the source eight-phase seasonal climate, QG weather and moisture sequence.
Its portable transforms, RNG and sparse solve retain source order and physical
units; the pinned SuperLU closure supplies the sparse factorization without a
platform BLAS or Python runtime. Wind, currents, precipitation and snow remain
computed fields rather than latitude-band substitutes. Cancellation propagates
through weather and thermal work without publishing a partial climate result;
progress reports completed stages.

MC-AI-0558 closes this local component prerequisite. The integrated three-seed
95-field comparison and Windows generation packet establish bounded evidence,
not universal numerical equivalence. The accepted q-only bound does not qualify
all model fields. Untested seeds, full numerical-chain bounds, all-domain L0–L3
coverage, Windows/Android canonical parity, performance and full game runtime
acceptance remain open under MC-AI-0483.

## Bounded materialized prefetch

Authoritative readiness covers 40 chunks (640 blocks) of horizontal interest:
81×81 profiles, each describing 64 vertical chunks. Protected interest is 768
blocks, or 97×97 profiles per client (9,409; 37,636 across four clients).
Profiles include exact highest-solid/layer/material data and dense edits;
eviction removes readiness. An ordered index preserves oldest-unprotected
eviction order without full-cache scans. Dense collision/start materialization
covers 48 blocks with a 32-block lead, bounded to 1,024 chunks per client.
Resident lead chunks promote without retransmission; see [GAMEPLAY.md](GAMEPLAY.md).

## Shared surface sampling

The worker pool reuses immutable completed 16×16 tiles across preview and
materialization. Its LRU holds 8,464 tiles (four 352-block interests at 46×46
XY); payload is about 83 MiB before index/allocator overhead. Keys bind terrain
identity, epoch, regional leaf and XY. Validation and cancellation precede
lookup/insertion; concurrent misses may duplicate work. Identity ignores
unchanged dense-cache churn but includes edits, deletion and profile replacement.
Cached samples alone never establish Ready.
