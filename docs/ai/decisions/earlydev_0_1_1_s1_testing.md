# S1 — Shared headless and 2D testing framework

The owner requires each current subsystem to have a deliberate correctness
and performance seam. Use one scenario/replay description and production
services; choose the presentation adapter and observation probes. A separate
test generator, height-only surrogate or instant-success renderer is not the
requested normal-game 2D mode.

## 2D normal-game adapter

Keep the normal game clock, inputs, authoritative world, workers, refinement,
collision, networking, budgets and chunk publication. Replace 3D rendering
with a CPU column-color adapter. Each world XY block column maps to one pixel
in bounded in-memory texture tiles. Read the highest non-Air block and its
canonical palette color from actual published chunk content; shared surface
metadata is usable only after its agreement with those chunks is established.
Unknown/unready columns have a distinct coverage mask, not invented terrain.
Track uniform-chunk classifications through the same content contract.
The chunk containing the canonical highest solid must be Ready before its
pixel is final; a lower loaded block cannot impersonate an ungenerated top.

Use explicit world-coordinate windows/tiles rather than allocating a 65,536²
texture. Retained old regions are separate tiles. Compare raw integer RGBA,
material IDs and coverage separately; omit lighting, labels, axes, compression
and antialiasing from golden images. Normal Vulkan remains the visual/FPS gate.

## Identity and pixel oracles

Version plus seed and complete terrain configuration define the world.
Thread count, queue order, camera direction, request/completion order,
movement, teleport history and eviction must not change any generated block.
CoreLang patch-pin/backend changes must preserve that output unless the
terrain generator version changes explicitly. Never silently regenerate
goldens to accept changed geography under the same version.

Compare the native headless exporter and 2D game mode with raw Python reference
material/height/color fixtures for the six ordered seeds. Extract numeric data
from the retained prototype; annotated report PNGs and blue diagnostic oceans
are not top-block-color goldens. Freeze coordinates, sample resolution,
palette, field units, fixture hashes and version/configuration identity.
Check all compared pixels, with mismatch coordinates and diff images.

The old implementation proposal to replace global PCG64 streams with a new
random sequence and simply regenerate the corpus is withdrawn. Specify the
retained model's random streams, rounding and math before porting. Require
reference pixel equality plus native cross-platform field/material digests.
If exact reproduction exposes a numerical incompatibility, resolve it in the
versioned specification; do not bless a changed world as the same Terrain-v2.

## Streaming topology and replay

The connectivity invariant concerns generated **coverage**, not landmasses:
legitimate ocean islands remain valid. Chunk results may finish in any order
privately, but newly published Ready chunks must join the active player's
face-connected chunk component through canonical wrapped neighbors. Seed
that component at a new spawn/teleport destination, then grow its frontier.
Old components may remain after relocation; label their generation epoch and
exclude them from current-radius completeness. Stale completions cannot
publish a new detached fragment. Check the top-view coverage mask as well as
the 3D Ready graph; Air is neither unknown nor solid ground.

Replay stationary spawn, x20/x30/x200 travel, diagonals, reversals, look turns
while moving, repeated/random seeded teleports and wrap crossings. Run the
same paths with reordered completions, one/many workers and forced eviction.
Compare final canonical content and common Ready pixels regardless of arrival
history; check connectivity after each publication and sphere coverage each
movement step. Camera direction may affect visual priority, never content or
the mandatory sphere. Record starvation/cancellation and time-to-catch-up.

## Subsystem evidence matrix

| Subsystem | Correctness / custom mode | Performance evidence |
|---|---|---|
| Configuration and CoreLang | Invalid identity/host input; interpreter/JIT/AOT output parity and exact dependency provenance. | Compile/load/call costs and allocation bounds. |
| Global geology/water/climate | Six-seed raw fixtures, causality, seams, routing and climate budgets. | Stage/total time, live/scratch/retained bytes. |
| Regional/local generation | Parent consumption, exact materials, chunk/sampler parity and regeneration. | Region/chunk throughput and tail latency. |
| Scheduler/cache | Randomized order, stale/cancel/dedup/admission/eviction; connected publication. | Queue/service latency, backlog, starvation and memory. |
| Simulation/collision/permissions | Headless input replay; spawn, movement and collision against actual chunks. | Tick costs and stalled/unready queries. |
| Protocol/replication | Loopback malformed/stale/late-join and two physical laptops. | Delivery/ack/tick latency, bytes and catch-up. |
| Loading/presentation | Same state machine with 2D capture; Vulkan screenshots and focus/cursor checks. | End-to-end deadlines; actual fresh-frame GPU gameplay. |

Use reusable clocks, trace recorders, field/pixel comparators and failure
artifacts, with explicit production constructors. Existing unit/subsystem
suites remain useful; their mocks do not close whole-game acceptance.

Classify every suite/check and select routine validation under the shared
[project iteration and validation policy](project_iteration_workflow.md). Its
budgets and failure-first reruns apply to both MinecraftClone and CoreProject2026;
this note records the design, not implemented selector support.
