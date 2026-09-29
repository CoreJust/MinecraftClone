# Deterministic scenario scripts

Scenario sources are loaded once and lowered to an immutable authoritative
plan. They are not evaluated in a render frame or server tick. The source
header selects the CoreLang frontend:

- Player scenarios begin with `@version("0.1.2")`, use `@use minecraft`, and
  export `pub fn scenario()` for the `flight3d-v1` profile.
- The bounded `sparse-world-v1` profile uses `@version("0.1.3")` and observes
  seeded terrain without players or tick operations.

An unknown or mixed header is rejected.

Use this guide as the entry point:

- [Quickstart](QUICKSTART.md) shows a CoreLang scenario.
- [Language reference](REFERENCE.md) defines grammar and typed host calls.
- [Execution model](EXECUTION.md) defines loading, authority, ordering, and
  resource limits.
- [Diagnostics](DIAGNOSTICS.md) lists stable failure codes.
- [Examples](../../scenarios/README.md) lists checked-in sources.

Both versioned scenario contracts use the trusted packaged CoreLang ruleset.
The `0.1.2` contract supports ordinary CoreLang functions, conditions, and
loops for bounded multiplayer scenarios. It exposes typed calls for profiles,
players, movement, waits, and position expectations. The `0.1.3` contract adds
typed generator options and bounded block and resident-chunk observations.
Calls collect a private plan; publication happens only after compilation and
execution complete successfully.

The host bounds source bytes, statements, actors, total waited ticks,
operations, and expectation evidence. These are host configuration, not script
options. They bound accepted host operations and plan size, but do not promise
that arbitrary CoreLang control flow terminates: the ruleset has functions,
conditions, and loops, and this integration does not add a general instruction
fuel budget. Keep packaged scripts finite and use the host limits as resource
guards.

The `sparse-world-v1` profile reuses the existing `SparseWorld` and
`TerrainGenerator`; its host-configured resident-chunk bound applies while
observations traverse distant coordinates. See the [reference](REFERENCE.md)
for accepted calls and bounds.

World generation is also a separate load-time CoreLang source. The packaged
`scenarios/world/canonical_world.core` calls `terrain_random`, `set_block`, and
`publish` while building a private 16x16x16 candidate. A failed compile,
runtime call, or incomplete publication leaves the live world unchanged.

The packaged `height_tile.core` may declare `configure_generation()` at load time.
Version 1 registers optional world pre-generation, refinement extents from
16384, 4096, 1024, 256, and 64 blocks, ordered virtual-chunk stages, and a
unique distance/view/movement priority order. An absent entrypoint uses direct
materialization. Invalid stages, extents, or duplicate priorities reject the
plan before generation starts. Stage output is bounded and current revision
results publish only after the final configured stage.
