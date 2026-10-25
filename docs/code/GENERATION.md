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
blocks immediately beneath it. R6 uses Grass above −2 °C and Snow otherwise.
V9 preserves model height: exposed biome 5 is Sand; biome 0 is Snow only below
0 °C. Non-desert coast Sand requires dry land, native L0 ocean/lake adjacency
and elevation through 20 m above MSL; rivers alone do not qualify. LOD retains
these source fields. Climate queries return periodic bilinear temperature and
precipitation.

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

V9 uses Platec, terrain, climate and water. Vendor static segment/river
scratch requires wrapper-level serialization. Cancellation is rechecked after
locking; downstream stages run unlocked. Terrain remains in source metres;
climate remains physical. H0–H3 return metres. L0 is coarsest for overview; L3
is finest for surface, collision and edits. Game Z is
`floor(384 + source_m / 10)` (0–1023); out-of-domain queries fail, never
normalize per seed. Keep typed fields and solid columns then air; no
caves/overhangs. Memo saturation recomputes misses into caller-owned values.
Numeric bounds, L0–L3/seams, platform parity and performance remain MC-AI-0483
gates.

Default no-argument LocalGame requests V9. CoreLang's v2 plan omits a model tag
and reconstructs R6 from nine typed fields: seed, resolution, epochs, erosion
steps, initial/final climate iterations, currents, wind sign and coastal
elaboration. WorldPreparation maps only the plan tag, validates returned fields
and plan, restores V9 and checks the full original configuration before atlas
creation. The client uses the server's V9 session; failures do not fall back.

## Native V9 seasonal climate

Climate consumes the canonical 256×256 bed for source eight-phase seasonal
climate, QG weather and moisture. Portable transforms, RNG and SuperLU retain
source order and physical units without platform BLAS or Python. Wind, currents,
precipitation and snow come from these calculations. Cancellation prevents
partial climate publication; progress reports completed stages.

MC-AI-0558 passes bounded three-seed field comparison and Windows generation
tests; this is not universal numerical equivalence. The q-only bound does not
qualify other fields. Untested seeds, full numeric-chain bounds, all-domain
L0–L3, Windows/Android canonical parity, performance and game runtime remain
MC-AI-0483 gates.

## Native V9 water and biome

V9 water consumes seasonal forcing and a 256×256 source-metre bed for
equilibrium, routing, marine salt/ice, snow/firn and biomes. Periodic TIN ties
are retained; incomplete catchment or receiver closure fails. It reports
progress and observes cancellation. The MSVC wrapper is inventoried. Six GSW
files remain byte-identical under `third_party/gsw`; hashes and unresolved
upstream revision are recorded. Numeric margins and actual-predecessor L0–L3
seams remain MC-AI-0483 gates.

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
