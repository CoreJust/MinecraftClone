# EarlyDev 0.1.1:1 — Improved world generation and solid blocks

Status: **Terrain-v2 design finalized by the owner, 19 October 2026**;
S1 implementation remains planned. Terrain-v1 names the current repeating-wave
generator. The finalized [Terrain-v2 design](earlydev_0_1_1_s1_generation.md)
and its linked algorithm, architecture, rationale and acceptance notes are
the implementation specification. The
[product roadmap](../../ROADMAP.md#earlydev-011--basicblocks) owns the six
snapshots; this document elaborates S1. Prototype evidence is separate from
native game acceptance.

## Intended result and boundaries

Terrain-v2 replaces repeating stone waves with seeded tectonics, varied
geography and progressive detail. Climate selects grass/dirt or snow/ice over
stone and bottom bedrock. Oceans remain dry until S4. Normal gameplay must
show these materials and agree with server collision; showcases and chunk-only
fixtures do not satisfy S1.

Cursor capture, ordinary moving-frame performance and visible terrain service
are inherited defects, not S1 product features. Fix and regress them in the
first engineering lanes while independent terrain work proceeds; recheck
affected behavior on the final S1 world.
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
| S1-13 | Use CoreLang 0.1.5 with JIT/AOT; temporary exact ai-dev pin, then release. | Owning-chat handoff, backend/provenance/parity checks and version-safe upgrades. |
| S1-14 | Show actual world-map generation, refine safe spawn and generate its 48-block sphere; prompt for any key. | Normal loading state machine and rendered prompt within 4 s from T0. |
| S1-15 | Ready 320-block sphere within 20 s Mac / 15 s Windows from T0; preserve it at x30 with ≥99% moving time at ≥120 / ≥300 fresh FPS. | Exact coverage and raw timing/frame evidence at existing laptop settings. |
| S1-16 | Normal-game 2D top-block-color mode shares actual generation. | In-memory pixel and coverage comparisons across movement/order/teleport scenarios. |
| S1-17 | Two physical laptops, one local and one remote client; host on either OS. | Mac-host/Windows-host playtests, identities, correctness and separate laptop metrics. |
| S1-18 | Review/separate modules and elaborate subsystem correctness/performance testing. | Source audit, standalone/custom-mode consumers, strict Python/native pixels and same-version seed/config reproduction. |

Requirements S1-01–08 come from the owner's recovered S1 plan and subsequent
tectonics/climate clarification in the roadmap; S1-10/11 from the owner's
subsequent staged-generation and map request; S1-12 from the later explicit
coast/island-family and world-anisotropy clarification;
S1-09 comes from the explicit correction about fixes and the retained handoff.
S1-13–18 come from the owner's additional S1 requirements and 0.1.5, sphere,
settings and final T0 clarifications. Reproducibility, bounded work, authority/render agreement and seams derive
from existing generation/world contracts.

## Solid materials

The [material/column contract](earlydev_0_1_1_s1_materials.md) owns inclusive
highest-solid H, sea level S, exact four-block ground/seabed bands and bottom
Bedrock. It preserves the original boundary cases and climate-driven palette.

## Planning references

- [Finalized Terrain-v2 design and experiment index](earlydev_0_1_1_s1_generation.md).
- [Work packages and native acceptance](earlydev_0_1_1_s1_acceptance.md).

## Additional finalized contracts

- [Loading map, key gate and sphere readiness](earlydev_0_1_1_s1_loading.md).
- [Mac/Windows performance predicates](earlydev_0_1_1_s1_performance.md).
- [CoreLang 0.1.5 pin and backend integration](earlydev_0_1_1_s1_corelang.md).
- [Shared 2D/headless testing and deterministic pixels](earlydev_0_1_1_s1_testing.md).
- [Project review and module boundaries](earlydev_0_1_1_s1_modules.md).
- [Two-laptop multiplayer qualification](earlydev_0_1_1_s1_multiplayer.md).
- [Parallel three-major iteration plan](earlydev_0_1_1_s1_iterations.md).
- [Planned 4h/12h workflow for both projects](project_iteration_workflow.md).
