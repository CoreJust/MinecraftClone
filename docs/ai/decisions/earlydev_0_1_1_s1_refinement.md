# S1 — Regional elaboration and local chunks

The [global atlas](earlydev_0_1_1_s1_generation.md) fixes geography, water-body
constraints and climate. First-touch regional work adds progressively smaller
relief without recomputing global tectonics or climate. This is constrained
procedural elaboration with a bounded hillslope approximation, not a second
river-basin or sediment simulation.

## Canonical field

Retain the existing registered regional extents
`16384 → 4096 → 1024 → 256 → 64` blocks. At each extent, address a periodic
integer lattice by seed, level and wrapped XY node. Use the explicit 64-bit
mixing constants/shifts in the retained prototype, with modulo-2⁶⁴ arithmetic;
never use the process/library hash function. Detail amplitudes in geological
km are respectively `0.18, 0.09, 0.045, 0.022, 0.011`.

Each node takes one relaxation step from its immutable centre and four
neighbors. Global slope biases weights downhill; rainfall controls diffusivity
`0.15+0.18*tanh(P)`. Interpolate nodes with cubic smoothstep. Modulate amplitude
by parent slope, rainfall and coldness; damp it in the ocean and suppress it
in parent lake/river corridors. Child contributions vanish at the exact
coarser lattice corners. Query partition, cache state and request order must
not affect the field.

Preserve the interpolated parent's land/sea sign and exact fully masked
river/lake beds. New relief cannot close a mapped outlet or raise a lake bed
above its parent water surface. A one-node relaxation halo makes shared nodes
identical. This contract deliberately does not discover new small lakes or
reroute rivers at each level. Such additions need a later algorithm revision
with explicit parent outlet and water-balance constraints.

The Python prototype evaluates all five levels as one canonical function and
demonstrates seams/order independence. Production must additionally materialize
and consume the five typed dependency levels: a stage counter or unused
ancestor byte buffer is insufficient. Use 65×65 stored samples per regional
artifact, with a one-node calculation halo and exact parent metadata. Do not
let interpolation of a coarse stored grid replace the canonical leaf sampler:
the 16×16 final tile evaluates the field at actual block coordinates.
Provisional visual LOD samples the same field at its declared quality level;
authoritative heights always use all prescribed levels.

## Shared surface and voxel contract

Validate/freeze the atlas once. Queries must not scan or copy atlas-sized
arrays per chunk; the experiment's prepared immutable parent removes that
cost. Local work depends on the requested samples and fixed five-level
neighborhood, independent of atlas area and visited-world extent.

Map geological elevation into blocks with one world-wide sea Z and vertical
scale, then floor to highest solid `H`. Reserve Z=0 for Bedrock and standing
clearance above maximum H. Never normalize individual regions. The prototype
uses S=384, H in `[1,1020]`, and a scale fitting global geological extrema into
the available 383/636-block depths/heights. Detail is bounded before the final
clamp; retain clamp counts so saturation cannot hide lost mountain relief.

`sampleSurface` returns H, diagnostic water height/presence, temperature/rain,
top and ground material. `materializeChunk` fills one 16³ palette from the
same 16² columns and [exact layer intervals](earlydev_0_1_1_s1.md). Z never
wraps. S1 materialization returns Air above H even where the map depicts
water. Physical water, swimming and flow remain S4.

Use identical surface data for server collision, streamed previews, cliff
layers and headless detailed maps. Preserve global constraints in every
eviction/regeneration path. Tests cover opposite seams/corners, adjacent
regions queried in both orders, independent regeneration, exact ancestor
anchors, materialized/sample parity, cold layers and S−4/S−5 seabeds.

The [architecture contract](earlydev_0_1_1_s1_architecture.md) makes regional
outputs real dependencies and repairs current default-generator bypasses.
Measured regional/column/chunk costs are in the
[experiment report](earlydev_0_1_1_s1_experiments.md); these Python costs do
not establish gameplay FPS or a native worker budget.
