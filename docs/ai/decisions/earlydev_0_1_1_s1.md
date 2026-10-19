# EarlyDev 0.1.1:1 — Improved world generation and solid blocks

Status: planning draft, 19 October 2026. The [product roadmap](../../ROADMAP.md#earlydev-011--basicblocks)
owns the owner's six-snapshot scope. This document elaborates S1; proposed
design choices below are not additional owner requirements or runtime evidence.
The current planning checkout is `s7-delivery-recovered`, `ai-dev`, inspected at
`ae70c623d21eefda1bce3211d5186e6e8389d467`. Preserve the older dirty checkout.

## Intended result and boundaries

S1 replaces the repeating stone-only wave landscape with recognizable oceans,
continents, plains, hills and grouped mountain ranges, with grass/dirt and
snow/ice surface layers over stone and bottom bedrock. Oceans remain dry basins
until S4. A normal player must see these materials and the same terrain that
the server uses for collision; a separate showcase or chunk-only fixture does
not satisfy the snapshot.

Cursor capture, ordinary moving-frame performance and visible terrain service
are inherited defects, not S1 product features. Fix and regress them as the
first engineering batch, then recheck affected behavior on the final S1 world.
The handoff's historical qualification does not establish a clean baseline.

Keep the existing 65,536 × 65,536 horizontal extent and 1,024-block vertical
extent, sparse residency, player-position-driven interest and projection-based
LOD. X and Y wrap; Z is bounded. Do not allocate the whole voxel world or tie
collision authority to client visual residency. S1 does not add block-edit
input, official mode presets, the S3 crosshair, water, player animation or bots.
Do not require an editable-world rewrite merely to generate layered S1 terrain;
S2 will need arbitrary voxel edits, including changes a height alone cannot encode.

## Ordered owner requirements

| ID | Requirement | Observable evidence required at implementation |
|---|---|---|
| S1-01 | Grass, dirt, bedrock, snow and ice join existing stone. | Stable block identities and distinguishable normal-game surface/cliff materials; invalid IDs rejected before mutation. |
| S1-02 | Exactly one bedrock layer at the world bottom. | Bedrock at Z=0 for every generated column; higher layers follow the rules below. |
| S1-03 | Slowly varying continental octave noise plus exponentially shaped local octave noise. | Fixed-seed whole-world elevation preview and ordinary travel show land/ocean shapes and local relief. |
| S1-04 | Mountains predominantly group into ranges, with lower mountains, hills and plains nearby; isolated mountains remain possible. | Several preselected seed previews and traversals show connected ranges and graded foothills rather than independent spikes. |
| S1-05 | Roughly half of terrain lies below sea level. | Record land/waterline-area statistics for each acceptance seed and the sampling method. |
| S1-06 | Land has one topmost block, four ground blocks and stone down to bedrock. | Exact boundary-column tests and visible cutaways/steep cliffs agree. |
| S1-07 | Each outer 20% north/south band uses snow/ice; the central 60% uses grass/dirt. | Tests at both band boundaries and wrap seam; latitude/material map and normal-game views. |
| S1-08 | Below sea level, ground occurs only in the four-block band immediately below sea level; lower terrain is stone. | Exact shoreline/deep-basin column cases below. |
| S1-09 | Fixes accompany the snapshot; they do not replace its product scope. | MC-AI-0477/0478/0479 evidence and representative final-S1 gameplay evidence. |

Requirements S1-01–08 come from the owner's recovered S1 plan in the roadmap;
S1-09 comes from the explicit correction about fixes and the retained handoff.
Seed reproducibility, bounded work, common authority/render semantics and seam
continuity below are derived from the existing generation/world contracts.

## Exact column rules

Proposed indexing convention: `H(x,y)` is the Z coordinate of the highest
solid block, and `S = SEA_LEVEL` is the highest Z that S4 may fill with water.
Thus `H >= S` is land, and `H < S` is submerged terrain. This convention must
be shared by generation, rendering, collision and S4 water placement.

At `z=0`, always generate Bedrock. At `z>H`, generate Air in S1. For the
remaining solid blocks (`1 <= z <= H`):

| Column | Layer | Block |
|---|---|---|
| Land: `H >= S` | `z == H` | TOPMOST_BLOCK |
| Land | `H-4 <= z < H` | GROUND |
| Land | `z < H-4` | Stone |
| Submerged: `H < S` | `S-4 <= z <= H` | GROUND |
| Submerged | `z < S-4` | Stone |

The bedrock rule takes precedence if a configured shallow column would overlap
the bottom. TOPMOST_BLOCK/GROUND are Grass/Dirt centrally and Snow/Ice in the
polar bands. Submerged ground therefore also changes to Ice in polar bands;
no grass/snow cap is generated below sea level.

Boundary examples, assuming enough depth above bedrock:

| Highest solid block | Result immediately below/at sea level |
|---|---|
| `H=S` | Topmost at S, four ground blocks at S-1 through S-4. |
| `H=S-1` | Four ground blocks at S-1 through S-4; Air at S until S4. |
| `H=S-3` | Two ground blocks at S-3 and S-4. |
| `H=S-4` | One ground block at S-4. |
| `H=S-5` | Stone-only column above bedrock. |

This reads “at most four blocks below SEA_LEVEL” as a fixed elevation band,
not four ground blocks beneath every seabed. It is the proposed precise reading
of the owner's shallow/deep-water rule and remains visible for correction.

Proposed latitude convention: normalize Y into `[0,65536)`, then let
`v=Y/65536`. Polar bands are `v<0.2` and `v>=0.8`; the rest is central.
Use that fractional predicate directly so integer-boundary rounding is explicit.
X has no effect on the band. Both ends of the Y seam have the same polar
materials. This is a latitude-band rule on the existing wrapped world, not a
claim that the world has spherical poles. Topmost and ground blocks are full
voxel blocks in this draft; snow layers, melting and ice friction are not
inferred from the block names.

## Planning references

- [Proposed generation and production integration](earlydev_0_1_1_s1_generation.md).
- [Work packages, acceptance and next decisions](earlydev_0_1_1_s1_acceptance.md).
