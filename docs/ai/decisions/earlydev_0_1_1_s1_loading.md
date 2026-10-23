# S1 — Loading map and readiness

Owner requirements added after Terrain-v2 model selection. The normal game
must show the literal generated world map during preparation, then prompt
**Press any key to start**. The standalone map exporter remains a separate
CPU test/demo entry point. A navigable map during ordinary gameplay is not
part of this requirement.

## State and clocks

Use `Creating → PreparingAtlas → PreparingSpawn → ReadyForStart → Playing`,
with explicit failure/cancellation states. Start monotonic `T0` when the
world-create/load request is accepted, before world-specific compilation,
generation or transfer. Initial readiness includes the global map, spawn-region
refinement, safe spawn and the minimal generated neighborhood. Show the ready
prompt within **4 seconds**. The player's key wait is not generation time.

The full-radius deadline also starts at **T0**, as clarified by the owner's
final correction: **20 seconds on Mac, 15 seconds on Windows**. Generation
continues while the ready prompt is visible and during gameplay. Pressing a
key neither resets these deadlines nor discards useful background work.
Record `T0`, atlas completion, spawn refinement, initial coverage, prompt's
first rendered frame, key acceptance and full-radius completion separately.
For a joining client, time its request-to-ready flow, including required map
and chunk transfer; distinguish server creation from joining an existing world.

Do not accept gameplay movement/look before the key gate. The world-generation
and network services continue. Transition input is consumed by the gate;
capture the cursor when entering Playing and retain focus/refocus/exit tests.
The current join/PlayerClient path starts automatically and must be changed.

## Exact spatial readiness

The owner clarified the initial neighborhood as a **3D sphere of radius
48 blocks** around the player. Apply the same geometry to the 20-chunk
playable radius: **320 blocks** for 16-block chunks. This is a volume, not
only a surface preview or horizontal disk.

Enumerate wrapped chunk AABBs intersecting the closed sphere, clipped to
bounded Z; use squared minimum AABB-to-player distance ≤ radius². Keep the
sub-block player position and use the nearest periodic XY image. Every key
must be Ready under the exact world identity, with authoritative block
content or a proven equivalent uniform chunk representation. Air counts as
generated space only after canonical classification; unknown data does not.
Surface previews alone cannot satisfy readiness or authorize collision.

Validate spawn support, clearance and permitted movement against the same
world before publishing the prompt. The initial sphere is centered on that
spawn. The playable sphere tracks the actual player position at each tick;
tests must check exact keys, including chunk corners and world wrapping.

## Visible map progression

Render actual staged geographical outputs: initial crust/height, evolved
coasts and water bodies, final climate/materials, then spawn refinement and
generated-coverage overlays. Label the current phase, elapsed time and real
completed/total work. Do not present a spinner or invented percentage as the
world map. Before a phase has usable data, show its honest pending state.

Workers emit throttled immutable map snapshots/deltas; the presentation thread
uploads textures. Keep at most one pending preview snapshot, replacing older
diagnostics rather than accumulating copies or blocking generation. These
diagnostics never publish a partly built atlas as
authoritative. The global image is coarse atlas detail; the spawn overlay can
show finer samples and actual Ready coverage. Water colors represent map
metadata while S1 game basins remain Air. Keep map rendering/copying inside
the measured four-second path and its memory accounting.

Cancellation releases request subscriptions and prevents stale snapshots,
late readiness or movement in an invalid world. Errors retain an actionable
creation/join message. Tests inject delayed/failed stages and stale epochs,
then exercise the same loading gate with both presentation adapters.
