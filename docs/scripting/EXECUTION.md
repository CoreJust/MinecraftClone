# Scenario execution model

## Load and publication

`parseScenarioSource` checks scenario and execution limits, checks source bytes,
observes cancellation, and accepts only the bare `@version(0.1.5)` CoreLang
header. It compiles with the trusted `minecraft` ruleset, statically validates
the bytecode, then executes `scenario`.
Host calls append to a private `ScenarioPlanCollector`. Compilation, runtime,
host-call validation, and the final required declarations must all succeed
before an immutable plan is returned. A failure cannot mutate the live world,
network state, or authoritative runner.

For `flight3d-v1`, the runner creates boundary-zero state, with declared players
and stopped input. Commands are ordered by source position. An input recorded
at boundary `B` is effective on the next authoritative step (`B + 1`) and
remains active until replaced. `wait(N)` advances exactly `N` fixed
authoritative steps and moves the boundary forward by `N`. `expect` records an
observation at the current boundary and does not advance time. Camera input is
converted from the actor's yaw into the authoritative XYZ direction; the server
remains the authority. Replay identity excludes wall-clock duration.

The `sparse-world-v1` profile uses the `0.1.5` frontend and has no players or
tick operations. After `profile` and `seed`, `sparseWorldOptions(u32, u64)`
selects generator version `1` and a positive resident-chunk bound. The bound
cannot exceed the host limit. `expectBlockXYZ(i64, i64, i64, u8)` materializes
and checks Air (`0`) or Stone (`1`) at a world coordinate;
`expectResidentChunks(u64)` checks current residency. Each observation counts
as one operation and one evidence item. Execution reuses `SparseWorld` and
`TerrainGenerator`; distant block observations evict chunks under the selected
residency bound.

## Host bounds

The application scenario command currently supplies these limits:

| Limit | Value | Applies to |
| --- | ---: | --- |
| Source bytes | 65,536 | Entire source before parsing |
| Statements | 256 | Every accepted host-call statement |
| Actors | 4 | `playerXYZ` declarations |
| Total ticks | 10,000 | Sum of all `wait` values |
| Operations | 512 | Emitted inputs, waits, expectations, sparse-world options, and observations |
| Evidence | 128 | `expectXYZ`, `expectBlockXYZ`, and `expectResidentChunks` operations |
| Resident chunks | 128 | Maximum accepted `sparseWorldOptions` bound |
| Bytecode bytes | 65,536 | Compiled module size |
| Static instructions | 65,536 | Proven upper bound for module initialization and `scenario` |
| Native image bytes | 4,194,304 | Compiled AOT image, when AOT is selected |

The API accepts other positive host configurations. Script input cannot raise,
lower, or replace them. The bounded source analyzer accepts functions and
conditions only when their compiled call and control-flow graph can be proven
within the static instruction limit. `for` loops require matching nonnegative
integer literal bounds and an immutable induction variable that is not changed,
aliased, or captured. Dynamic loops, recursive or indirect calls, static
initialization, and cycles outside a proven finite `for` range fail validation.
The bytecode checker includes module initialization and the `scenario`
entrypoint in its upper bound, so accepted control flow has a finite instruction
budget. The 30 second command watchdog remains a separate application deadline.

## Cancellation and errors

Cancellation is checked before frontend selection, during bounded compilation,
at execution phase boundaries, and around registered host calls. If observed,
lowering returns `cancelled` and publishes no plan. The static source and
bytecode checks reject control flow outside the bounded subset before runtime.

Invalid limits, source size, compilation, typed host arguments, ranges,
duplicate actors, operation budgets, tick budgets, evidence budgets, and final
required declarations fail before publication. See [Diagnostics](DIAGNOSTICS.md)
for stable codes.

## Seeded world loading

World generation uses a separate load-time CoreLang program and private
candidate. `canonical_world.core` iterates a 16x16x16 chunk, calls the trusted
seeded `terrain_random`, writes only stone block id `1` where
`z - 6 + random < 0`, and calls `publish`. The world loader rejects invalid
options, source overflow, compilation/runtime failure, host-call overflow, or
incomplete publication; the live world is changed only by a completed chunk
and its resulting configuration identity.
