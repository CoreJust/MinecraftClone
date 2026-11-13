# Native Terrain-v2 generation

`mc_generation` owns deterministic CPU kernels/configuration;
`mc_world_sampling` adds canonical regions, columns and chunks; `mc_world_map`
exports gameplay sampling. None requires window, renderer, network or CoreLang
startup. RGB diagnostics use fused multiply-add.

R6 snapshots are immutable; skipped callbacks preserve final fields. Validate
before allocation; cancellation/failure are explicit. V9 adds optional
wind/current (m/s) and expands eight-phase climate from 32² to 256².

Canonical regional elaboration inherits parent samples. `TerrainWorld` provides
typed columns and builds chunks; [GAMEPLAY.md](GAMEPLAY.md) covers
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
Fixtures bind seeds/hashes; comparisons do not approve expectations. Six-seed agreement does not prove universal host-libm equivalence.

## Nearest-land prepared spawn

Prepared Flight picks the nearest dry integer origin to `(32768,32768)`, ordered
by wrapped distance, squared distance, Y, then X. Require dry
`highest_solid_z + 1`, valid bounds, materialized support, body clearance and
no overlap. Toroidal lower-bound search polls at most 128 items; height/collision
stay asynchronous. Exhaustion fails without fallback.

`hasWaterPresenceThroughoutAtlasCell` prunes only complete aligned 256/512 R6
cells. R6 selects `floor(coordinate * resolution / 65536)` and carries its
ocean/lake/river flag through refinement. The final pre-seam column is included;
false, mixed or clipped cells require exact checks.

## Layered terrain LOD model

R6 visual `lod-canonical-r6-height-v2` is distinct from Terrain-v2 R6.
Periodic L0, 4×4 L1/L2 and canonical-column L3 share constrained seams.
Complete captures replace one 16-block L2 cell at resolution 256 or four
8-block cells at 512; gaps use fallback. V10 reuses source-bound 2 m fans;
authority stamps rebuild separately.

V9 combines Platec, terrain, climate and water. Vendor segment/river scratch is
serialized; cancellation is rechecked after locking, then downstream stages run
unlocked. Heights and H0–H3 stay in source metres. L0 is overview; L3 serves
surface, collision and edits. Game Z is `floor(384 + source_m / 10)` (0–1023);
out-of-domain queries fail. Columns retain typed fields and solids below air;
there are no caves or overhangs. Memo misses recompute into caller storage.
V10 [surface cover](V10_SURFACE_COVER.md) keeps shallow-ocean Sand within 20 m;
inland/deep beds retain Dirt.

LocalGame and Server enable V10 fine terrain.
`WorldPreparation` restores the shared CoreLang R6 fields. Local clients reuse immutable L0. Remote clients prepare from session identity.
The client worker prepares default S2 imagery and V10 rivers with world/L0
for loading and play. The main thread uploads completed rasters. Wind, currents and
precipitation reuse the existing raster.

Benchmark `--terrain-model v10` selects the f775 foundation with the c8f9b851
river field. `--terrain-model native-v10-fine` adds the fine field and appends
`;fine=e52e2e82b3e9873e14e543e0a7583fb978a182f1555997bda6febcd9ff25061a`
to the configuration identity. Seed-42 f775 prepares lazily with a 65,536 m period. Canonical V10 surfaces, climate, water queries, maps, tiles, chunks and
meshes use the configured bed. See [river beds](RIVER_BEDS.md) for signed inland relief, protected contacts
and final map composition. S2 changes the bed. S3 covers physical water and floating ice.

V10 uses isolated PyPlatec 1.4.3 (seed 42/resolution 256, sea 0.65, ten plates,
two cycles). L/O/d/R2 feed continuous LOD-vertex evaluation. Geometry is
`floor(512 + height_m)` at depth 2048; climate uses tenfold height. Annual-mean
stocks and same-bed water drive materials; seasonal queries remain.
[Acceptance](../ai/tasks/MC-AI-0649.md) remains open.

## Native V9 seasonal climate

Canonical 256×256 beds drive eight-phase climate, QG weather and moisture.
Transforms, RNG and SuperLU preserve source order/units without BLAS/Python.
QG reuses damping coefficients while dt is unchanged.
Wind/current/rain/snow use these calculations; cancellation prevents partial
publication and progress reports completed stages.

MC-AI-0558 passes three-seed comparison and Windows tests. Other seeds, fields beyond q, numeric bounds, L0–L3, platform parity, performance and runtime remain MC-AI-0483 gates.

## Native V9 water and biome

V9 water uses seasonal forcing and the 256×256 source-metre bed for equilibrium,
routing, marine salt/ice, snow/firn and biomes. Periodic TIN ties remain;
incomplete catchment or receiver closure fails. It reports progress and observes
cancellation. Six unchanged GSW files retain hashes and upstream revision. Numeric margins and
actual-predecessor L0–L3 seams remain MC-AI-0483 gates.

## Bounded materialized prefetch

Readiness spans 40 horizontal chunks (640 blocks): 81×81 profiles with 128
vertical chunks each. Protected interest is 768 blocks, or 97×97 profiles per
client (9,409; 37,636 across four). Profiles bind exact surface/layer/material
data and edits; eviction removes readiness. Ordered indexing avoids full-cache
scans. Dense collision/start covers 48 blocks with a 32-block lead, capped at
1,024 chunks per client; resident lead chunks promote without retransmission.
See [GAMEPLAY.md](GAMEPLAY.md).

## Shared surface sampling

LRU shares 8,464 immutable 16×16 tiles (~83 MiB), keyed by terrain/epoch/XY.
R6 requires typed regional leaves; NativeV9 reads its completed atlas. Validate
identity, epoch, bounds and cancellation before caching/publishing; duplicate
misses may occur. Edits, deletion and profile replacement change identity.
Cached samples grant no collision/edit authority or readiness; current-session
profiles and complete sparse revisions remain required. Preview cancellation is
separate.
