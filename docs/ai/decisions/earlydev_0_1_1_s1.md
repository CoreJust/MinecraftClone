# EarlyDev 0.1.1:1 — Improved world generation and solid blocks

Status: design for later implementation, 19 October 2026. The
[product roadmap](../../ROADMAP.md#earlydev-011--basicblocks) owns the six
snapshots; this document elaborates S1. Prototype evidence is separate from
native game acceptance.

## Intended result and boundaries

S1 replaces repeating stone waves with seed-driven tectonic base heights,
simulated or credibly approximated. Different-sized continents, ranges, trenches and peninsulas emerge with
smooth/irregular/ragged coasts, varied island families and uneven world-scale
land concentration; detail is added only where needed.
Climate selects grass/dirt or snow/ice over stone and bottom bedrock. Oceans remain dry basins
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
S2 owns arbitrary voxel edits, which a height alone cannot encode.

## Ordered owner requirements

| ID | Requirement | Observable evidence required at implementation |
|---|---|---|
| S1-01 | Grass, dirt, bedrock, snow and ice join existing stone. | Stable block identities and distinguishable normal-game surface/cliff materials; invalid IDs rejected before mutation. |
| S1-02 | Exactly one bedrock layer at the world bottom. | Bedrock at Z=0 for every generated column; higher layers follow the rules below. |
| S1-03 | Seed-driven tectonic simulation or approximation produces base heights and varied continents, ocean trenches, islands and peninsulas; detail follows only if needed. | Base-height and final previews across seeds show emergent geography; noise cannot supply all the structure. |
| S1-04 | Mountains mostly form ranges grading into lower mountains, foothills and plains; isolated peaks remain possible. | Tectonic fields and traversals explain connected ranges and their surroundings. |
| S1-05 | Roughly half of terrain lies below sea level. | Record land/waterline-area statistics for each acceptance seed and the sampling method. |
| S1-06 | Land has one topmost block, four ground blocks and stone down to bedrock. | Exact boundary-column tests and visible cutaways/steep cliffs agree. |
| S1-07 | Simulated climate selects materials in S1, replacing the former fixed outer-20% polar bands. | Temperature/moisture/material maps, height/coastal effects, threshold and seam tests, normal-game views. |
| S1-08 | Below sea level, ground occurs only in the four-block band immediately below sea level; lower terrain is stone. | Exact shoreline/deep-basin column cases below. |
| S1-09 | Fixes accompany the snapshot; they do not replace its product scope. | MC-AI-0477/0478/0479 evidence and representative final-S1 gameplay evidence. |
| S1-10 | Global tectonics → terrain/water elaboration → climate, then progressive first-touch regional elaboration and local chunks. | Causal diagnostic layers, bounded costs and exact parent/child consistency. |
| S1-11 | Standalone world-map generation for tests/demos; ordered seeds 0, 1, 42, 777, 2004, 8800555. | Actual maps, timings/memory, identities and retained rejected attempts; future map CLI shares gameplay generation. |
| S1-12 | Coast/island diversity, anisotropic land concentration and sparse unusual terrain phenomena follow the seed/processes. | Actual coastal/island crops and half-domain land statistics across the corpus; no repeated uniform templates or mandatory landmark catalog in each seed. |

Requirements S1-01–08 come from the owner's recovered S1 plan and subsequent
tectonics/climate clarification in the roadmap; S1-10/11 from the owner's
subsequent staged-generation and map request; S1-12 from the later explicit
coast/island-family and world-anisotropy clarification;
S1-09 comes from the explicit correction about fixes and the retained handoff.
Reproducibility, bounded work, authority/render agreement and seams derive
from existing generation/world contracts.

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
the bottom. TOPMOST_BLOCK/GROUND follow the [climate material table](earlydev_0_1_1_s1_climate.md):
Snow/Ice when cold; Grass/Dirt when warmer and moist; proposed Dirt/Dirt when
warmer and dry. Submerged ground can therefore be Ice; no grass/snow cap is
generated below sea level. Inland lake beds above S retain the land rule.

Boundary examples, assuming enough depth above bedrock:

| Highest solid block | Result immediately below/at sea level |
|---|---|
| `H=S` | Topmost at S, four ground blocks at S-1 through S-4. |
| `H=S-1` | Four ground blocks at S-1 through S-4; Air at S until S4. |
| `H=S-3` | Two ground blocks at S-3 and S-4. |
| `H=S-4` | One ground block at S-4. |
| `H=S-5` | Stone-only column above bedrock. |

The owner's shallow/deep-water rule uses a fixed sea-level band, not four
ground blocks beneath every seabed.

Climate uses smooth toroidal insolation, elevation, ocean heat transport and
wind/moisture effects. There is no fixed Y-band material predicate. Snow and
Ice remain full solid blocks; thin snow, melting and friction are not inferred.

## Planning references

- [Selected generation model and experiment index](earlydev_0_1_1_s1_generation.md).
- [Work packages, acceptance and next decisions](earlydev_0_1_1_s1_acceptance.md).
