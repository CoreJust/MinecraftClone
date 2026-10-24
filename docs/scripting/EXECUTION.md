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
authoritative steps and moves the boundary forward by `N`. Each `jump` queues
one jump input and advances the tick counter by one. All other inputs,
permission changes, and expectations leave the boundary unchanged.
Expectations record observations at the current boundary. Camera input is
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
| Statements | 256 | Every scenario host-call statement, including `profile`, `seed`, and player declarations |
| Actors | 4 | `playerXYZ` declarations |
| Total ticks | 10,000 | Sum of every `wait` value plus one per `jump` |
| Operations | 512 | Collected movement inputs, permission changes, jumps, waits, expectations, `sparseWorldOptions`, block observations, and residency observations |
| Evidence | 128 | Every `expectXYZ`, `expectMovementPermissions`, `expectVerticalVelocity`, `expectBlockXYZ`, and `expectResidentChunks` call |
| Resident chunks | 128 | Maximum accepted `sparseWorldOptions` bound |
| Bytecode bytes | 65,536 | Compiled module size |
| Static instructions | 65,536 | Proven upper bound for module initialization and `scenario` |
| Native image bytes | 4,194,304 | Compiled AOT image, when AOT is selected |
| Runtime executions | 2 | Maximum number of calls to the scenario entrypoint |
| Command watchdog | 30 seconds | Application deadline for a scenario command |

These are the application command's defaults; hosts may supply another
positive configuration. Script input cannot change the selected host limits.
The counters apply at different points while lowering the source:

| Stage | Counter | What consumes it | What does not consume it |
| --- | --- | --- | --- |
| Source admission | Source bytes | Source bytes before parsing | Nothing inside the script can raise this limit |
| CoreLang collection | Statements | Every registered host-call statement, including `profile`, `seed`, and `playerXYZ` | Ordinary CoreLang statements that are not host calls |
| Plan collection | Operations | Each operation emitted by an input, permission change, jump, wait, expectation, sparse-world configuration, or sparse-world observation | `profile`, `seed`, and `playerXYZ` declarations |
| Plan collection | Total ticks | `wait(N)` contributes `N`; each `jump` contributes `1` | Movement inputs, permission changes, and expectations |
| Plan collection | Evidence | Each position, permission, velocity, block, or residency expectation | Inputs and `sparseWorldOptions` |

Every counter is checked before an immutable plan is published. A wait that
would cross the tick limit, an emitted operation after the operation limit, or
an expectation after the evidence limit fails the whole lowering; no partial
plan reaches the runner. Sparse block observations count as one operation and
one evidence item apiece, as does each residency expectation. The configured
resident-chunk bound constrains the later sparse-world observations; it does
not grant script code a way to resize the host world.

The default bounded CoreLang execution uses the interpreter, limits the
scenario source to 65,536 bytes, compiled bytecode to 65,536 bytes, the proven
static instruction count to 65,536, and an AOT image (if selected) to 4 MiB.
The shared API retains a 65,536 default instruction limit and accepts explicit
limits up to 1,048,576 instructions for bounded callers with a measured need;
the scenario command continues to use 65,536. Persistent function-local
statics are rejected by default. A loader may opt into them only when its
preflight counts the full initializer path on every entry.
The source analyzer accepts functions and conditions only when their compiled
call and control-flow graph can be proven within the instruction limit. `for`
loops require matching nonnegative integer literal bounds and an immutable
induction variable that is not changed, aliased, or captured. Dynamic loops,
recursive or indirect calls, static initialization, and cycles outside a
proven finite `for` range fail validation. The bytecode checker includes
module initialization and the `scenario` entrypoint in its upper bound. The
30-second watchdog is a separate application deadline around the scenario
command.

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

World generation uses separate load-time CoreLang programs and code paths;
the scenario limits above do not apply to them. Those adapters use bounded
compilation and prove each called entry before the first runtime call.
`ScriptedWorld` uses a 262,144-instruction limit; `TerrainGenerator` uses a
cumulative 65,536-instruction limit. They do not have the scenario command's
30-second watchdog, runtime fuel, or execution cancellation. See
[World generation](WORLD_GENERATION.md) for their exact callable contracts,
source limits, and remaining trust boundary.
