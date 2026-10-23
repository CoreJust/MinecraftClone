# S1 layered terrain scaffold

The latest owner amendment defers the other-chat terrain-model replacement to
S2. S1 keeps existing Terrain-v2 R6, simpler L1/L2/L3 and integrated curvature.
Tests, original deadlines, 99% moving-FPS thresholds and owner playtest remain
required. Exact CoreLang0.1.5 ai-dev is acceptable for this snapshot; no final
language release wait is required.

## Ordered acceptance

1. Periodic 65,536-square-block world; L0 exactly 256 or 512 samples per axis. L1 refines each L0 cell 4×4; L2 refines again 4×4; L3 uses actual authoritative chunks. Default implementation choice: 256.
2. A separately identified placeholder uses the accepted global generator and deterministic coordinate/seed/level residuals, with bounded mountain shaping. Parent anchors, shared edges, water and categorical identity remain stable. Frozen R4/R5 and canonical R6 outputs remain unchanged.
3. Complete distant L0 coverage remains visible during refinement. Parent replacement occurs only after a complete child scene or actual chunk surface group is prepared and uploaded. Failed/stale uploads retain the previous complete scene. No duplicate surface or uncovered replacement footprint.
4. Owner follow-up selects continuous linear triangle terrain instead of center-height terraces. Equal/mixed-level and periodic boundaries share canonical edge chains. Actual boundaries follow one-block authoritative steps with explicit closure; incomplete vertical capture cannot expose holes. Collision, edits, caves and overhangs remain authoritative block data.
5. Initial global generation target ≤6 seconds and ≤2 GB; global plus L1/L2 touching the literal central 256×256 block area target ≤10 seconds and ≤2 GB, excluding actual chunk generation. Export the central heightmap, biome mask and retained artifacts. Do not relocate an ocean center. Report exact excess; no numeric tolerance was granted.
6. Owner correction requires normal authoritative generation/preservation over horizontal40chunks=640blocks using full-volume column profiles. Prior40/72-block implementation was an assistant unit error, preserved as invalid historical evidence. Height-only640 over40 actual is insufficient. No corrected Ready claim until full implementation and ordinary play.
7. Verify 256 and 512 contracts, shared edges/anchors, cancellation, epoch replacement, failed upload retention, actual-chunk handover and ordinary remote-client rendering. Measure production frame/streaming impact and required platform budgets independently.

## Design and integration

Regional scheduling extents remain separate. L0/L1/L2 cell sizes are
256/64/16 blocks at resolution 256, or 128/32/8 at 512; actual chunks replace
four finest cells at 512.

Client source preparation asynchronously duplicates the session atlas and
rejects stale identity/epoch results; its cost remains measurable. The worker
builds complete scenes; separate GPU staging publishes atomically after upload.
Authoritative near data/collision remain; snapshots feed off-thread meshing.

## Owner follow-up: surface and curvature

Owner playtest11 failed movement detail/LOD flicker, coarse-only x15 terrain,
keyboard translation locks and join walls/height jumps. Interpolation is permitted. MC-AI-0510 selects linear triangles; quadratic interpolation is
unnecessary. The render contract is now `lod-linear-triangles-v2`; this is a
presentation contract, not a new canonical Terrain-v2 model or numerical fixture.
The placeholder generator identity remains `lod-scaffold-s1-placeholder-v1`.

Triangles reuse the 32-byte face payload with direction 6, packed 1/32-block
heights and signed local XY offsets. Flagged top triangles also pack three
1/32-degree temperatures. Perspective-correct fragment interpolation of height
and temperature selects Stone below sea−4, Dirt/Ice below sea, and Grass/Snow
on land using the −2 °C cold threshold. Climate endpoints follow the same
canonical edge recursion as geometry. Stone/player/text push constants occupy
128/128/96 bytes; Stone carries the scene sea-level uniform. Actual-block
materials and far-L0 geometry/detail remain unchanged. The coastline follows a
piecewise-linear elevation contour on existing coarse triangles. Canonical coarsest-incident edge functions
constrain finer endpoints. L3 joins use exact unit-block edge heights and
conservative exterior curtains/bottom closure. These can conceal boundary caves;
they do not establish complete captured lower volume or prove the owner's
reported holes resolved without runtime replay.

MC-AI-0511 tracks the separate visual-curvature request. Authoritative world
coordinates, generation, movement and collision remain flat. The implementation
chooses paraboloid radius 131072 blocks: rendered Z decreases by horizontal
camera-relative distance squared divided by twice that radius. Terrain, actual
blocks, players and world labels share the transform while the LOD scene is active.
GUI and diagnostic scenes remain flat. Horizontal Euclidean visibility ends at
16384 blocks (1024 chunks), independent of eye Z, with fog from 80% of that radius.
Radius/fog are implementation choices.

The owner permits only L0 beyond 512 or 384 chunks; the acceptance ceiling is
512 chunks (8192 blocks). Current refinement is narrower: nearest-periodic-AABB
Chebyshev thresholds are 1024 blocks for L0 and 384 for L1. This satisfies the
outer L0-only ceiling without promising detail throughout that radius. Curved
CPU bounds use nearest/farthest horizontal distances; world seams preserve short
periodic deltas. Active-LOD projection expands its far bound for altitude, packed terrain heights
and maximum bend. Low/high-altitude captures still must establish actual visible
coverage; helper tests alone do not.

## Current evidence and limits

One seed42/256 Mac probe measured global+L0 5.78098 s, central L1/L2 5.78131 s, and peak RSS 41,779,200 bytes; this is not production/platform acceptance. Mesh regressions and isolated curvature/shader checks pass in local worker receipts. Final integrated Mac/Android captures, owner-view surface alignment, high-altitude cutoff, production performance and ordinary runtime qualification remain open.

The owner rejected the later block-shaped material boundary despite smooth
geometry. Bounded material tests remain evidence, but playtest11 failed; corrected full-volume
640-block coverage and ordinary owner acceptance remain open. Earlier responsive QA and high-altitude
observations do not accept this corrected candidate or establish representative
production FPS.
