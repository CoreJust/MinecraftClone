# Deterministic scenario scripts

Scenario sources are loaded once and lowered to an immutable authoritative
plan. They are not evaluated in a render frame or server tick. `parseScenarioSource`
accepts the bare header `@version(0.1.5)`, followed by `@use minecraft` and an
exported `pub fn scenario()`. Both `flight3d-v1` and `sparse-world-v1` are
profiles under this frontend; the profile determines which host calls are
valid. Quoted legacy selectors and unknown headers are rejected.

Use this guide as the entry point:

- [Quickstart](QUICKSTART.md) shows a CoreLang scenario.
- [Language reference](REFERENCE.md) defines grammar and typed host calls.
- [Execution model](EXECUTION.md) defines loading, authority, ordering, and
  resource limits.
- [Diagnostics](DIAGNOSTICS.md) lists stable failure codes.
- [Examples](../../scenarios/README.md) lists checked-in sources.

The `minecraft` ruleset exposes typed calls for profiles, players, movement,
waits, position expectations, and sparse-world observations. Calls collect a
private plan; publication happens only after bounded compilation and execution
complete successfully.

The host bounds source and bytecode size, static instruction count, actors,
waited ticks, operations, and expectation evidence. A source and bytecode proof
accepts functions and conditions only when execution remains within the
instruction limit. `for` loops require immutable induction variables and
matching nonnegative integer literals for their range; dynamic loops,
recursive or indirect calls, and cycles outside a proven finite `for` range
are rejected. Host limits also bound accepted plan size and sparse-world
residency.

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
