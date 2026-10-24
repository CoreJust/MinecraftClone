# World-generation scripts

World generation uses load-time CoreLang programs, separate from scenario
scripts. The packaged sources use the bare `@version(0.1.5)` header and
`@use minecraft`. Both public host registries use `minecraft` ruleset version 1
and host-call ABI major 1. Their calls return `unit` unless a result is stated.
The generation-plan manifest version is separately fixed at 1. The host
collects a candidate plan or chunk before publishing it.

## Scripted 16×16×16 chunks

`ScriptedWorld::load(source, options)` compiles a world script and invokes its
exported `generate` entrypoint with the single `u64` argument `options.seed`.
The packaged source is
[`canonical_world.core`](../../scenarios/world/canonical_world.core), where
`generate(seed: u64)` returns `unit`. The adapter requires successful runtime
completion but ignores the entrypoint's returned value. Its starting candidate
contains 4,096 Air blocks; a script may replace blocks before its one required
`publish()` call. It need not call `set_block` for every coordinate. A failed
script or missing publication discards the candidate.

| Call | Contract |
| --- | --- |
| `terrain_random(u64, i32, i32, i32) -> i32` | `(seed, x, y, z)`. Seed must equal the configured seed; x, y, and z are local chunk block coordinates, each `0..15`. Returns a deterministic fixture-noise sample from `-3..3`, represented as `i8` and widened to `i32`. |
| `set_block(i32, i32, i32, i32) -> unit` | `(x, y, z, block_id)`. Coordinates must each be `0..15`; block ID must be Air `0` or Stone `1`. May be called only before publication. |
| `publish() -> unit` | No arguments; must be called exactly once. `generate` must complete after it. A second publication, or a block write after publication, fails the script. |

`ScriptedWorldOptions` defaults are:

| Option | Default | Accepted values |
| --- | ---: | --- |
| `seed` | `42` | Full `u64` range; supplied to `generate` and must be echoed to `terrain_random`. |
| `chunk_coordinate` | `(0, 0, 0)` | The `ChunkCoordinate` stored on the resulting chunk; the options validator adds no coordinate range check. |
| `chunk_width`, `chunk_height`, `chunk_depth` | `16` each | Each must equal 16. Other chunk dimensions are rejected. |
| `max_source_bytes` | `8,192` | Must be positive; effective source cap is the smaller of this value and `65,536` bytes. |
| `max_host_calls` | `8,193` | Must be positive; counts every `terrain_random`, `set_block`, and `publish` callback, including a callback that fails validation. |

The three dimensions must be exactly `16×16×16`; a maximum of 8,193 callbacks
allows the default worst case of one random sample and one write per block,
plus publication. The callback limit bounds host calls;
`ScriptedWorldErrorCode` includes
`InvalidOptions`, `SourceTooLarge`, `CompilationFailed`, `RuntimeFailed`,
`HostCallLimitExceeded`, `InvalidHostCall`, and `IncompletePublication`.
Current `ScriptedWorld::load` reports invalid callback invocations through
`RuntimeFailed`; `InvalidHostCall` is declared but not emitted by this path.

## Height tiles and generation plans

`TerrainGenerator(script_source)` uses the packaged
[`height_tile.core`](../../scenarios/world/height_tile.core) when `script_source`
is empty. A custom source must export all three required parameter getters
below. `configure_generation() -> unit` is optional and, if present, must be
the single exported function of that name with no parameters. The host executes
it once during construction. The generated plan must include at least one
pre-generation declaration, refinement extent, or virtual chunk stage.

| Call | Contract |
| --- | --- |
| `generation_plan_version(u8) -> unit` | Required exactly once and first; only manifest version `1` is accepted. |
| `generation_pregen() -> unit` | Enables a pre-generation stage; no arguments; at most once. |
| `generation_refinement(u64) -> unit` | Adds one refinement extent, in descending order and without duplicates. Allowed extents are `16,384`, `4,096`, `1,024`, `256`, and `64` blocks; at most five. With pre-generation enabled, `16,384` cannot be the first refinement extent. |
| `generation_virtual_stage(u8) -> unit` | Adds a chunk stage: `1` is HeightTile, `2` is Materialize. HeightTile may occur at most once; Materialize may occur at most once and must be last. At most two stages. |
| `generation_priority(u8) -> unit` | Adds one unique priority dimension: `0` Distance, `1` View, or `2` Movement. At most three. If omitted entirely, priority defaults to Distance, View, Movement in that order. |

All five declarations share a maximum of 16 host-call invocations, including
the required version call. Calls other than the version may be arranged in any
order that yields a valid plan. An absent `configure_generation` uses direct
materialization, no pre-generation or refinements, and the default
Distance/View/Movement priority order. The generation-plan adapter has no
stable diagnostic-code enum; invalid declarations and failed script
execution surface as constructor exceptions.

The following exported getters are required and called with no arguments:

| Export | Type | Accepted result | Packaged value | Unit interpretation |
| --- | --- | --- | ---: | --- |
| `base_height() -> f64` | `f64` | Finite and `>= 0` | `6.0` | Inferred vertical block units; contributes the base to sampled height. |
| `crest_spacing() -> f64` | `f64` | Finite and `> 0` | `128.0` | Inferred horizontal block distance between terrain waves. |
| `crest_amplitude() -> f64` | `f64` | Finite and `>= 0` | `800.0` | Inferred vertical block units; contributes the wave amplitude. |

These values in the table come from the packaged source; there is no fallback
for a missing getter in a custom source. The height kernel samples world block
x/y, computes a radial wave height, and clamps the result to the world
depth (`0..1,024`). This describes the `TerrainGenerator` height-tile adapter,
separate from production `TerrainWorld` generation. Materialization fills blocks
below each sampled surface with Stone and blocks above it with Air. Sand at
desert and beach surfaces, and snow at cold summits are planned extensions that
depend on the new terrain model and are not part of the current materialization
contract.

Scheduler output bounds apply across generation stages: each stage result is
limited to 4,096 bytes; the queued/refinement output budget is configurable but
cannot exceed 1 MiB (the default); completed regional results are limited to
32 MiB; and retained regional artifacts are limited to 32 MiB. The scheduler's
default pending-job bound is 32. These limits constrain generated output and
retained work, not execution time or script instructions.

## Execution and trust boundary

Both source-taking adapters use `compileBounded` and statically prove the
source and bytecode size bounds before the first runtime call; compiled
bytecode is limited to 65,536 bytes. `ScriptedWorld::load` proves its single
`generate(seed)` call against a 262,144-instruction limit. The packaged
canonical script has a conservative static count of 235,824 instructions.
`TerrainGenerator` proves optional `configure_generation()` and each required
getter once, and requires their cumulative bound to fit within 65,536
instructions. The adapter reuses one Runtime, compiled program, and provider,
but each `Runtime::execute` creates a fresh CoreLang execution. Function-local
statics retain changes across calls within one entry and start from their
initial values in the next entry. Each entry proof includes module
initialization; the terrain total conservatively counts that cost for each
entry call.

The static checks reject dynamic `while`, `do-while`, and `loop` control flow,
recursive or indirect calls, and unproven bytecode cycles. Finite `for` loops
are accepted only when they use a named immutable induction variable that is
not written, aliased, or captured, with matching nonnegative integer literal
bounds; their work is included in the instruction bound. Terrain generation
opts into persistent function-local static state. Its bound counts the full
first-initialization path, including initializer calls, for every configured
entry; later getter calls can skip initialization but cannot exceed that
conservative count. Other bounded CoreLang callers reject persistent statics by
default. Scripted-world source is limited to the smaller of
`ScriptedWorldOptions.max_source_bytes` and 65,536 bytes (8,192 by default).
Custom terrain source is limited to 65,536 bytes; an empty source selects the
packaged source.

These checks give accepted entries a static instruction ceiling, but do not
provide a runtime deadline, instruction fuel, or cancellation while synchronous
CoreLang execution is in progress. The host-call and output limits are separate
bounds. Caller-supplied or editor-authored scripts should still be treated as
untrusted input; bounded preflight is not a general execution sandbox.

## Internal terrain-v2 manifest

The ten `minecraft.terrain_v2.*` operations below belong to the generated
terrain manifest in `src/shared/corelang/Backend.cpp`. They are internal
manifest plumbing, not supported general-purpose script calls and not public
promises. Their private ABI major is 1; the fixed manifest version is 2.

| Private operation | Signature | Internal value or check |
| --- | --- | --- |
| `terrain_seed` | `terrain_seed() -> u64` | Frozen configuration seed; default `42`. |
| `terrain_resolution` | `terrain_resolution() -> u32` | Atlas resolution; default `256`, accepted `256` or `512`. |
| `terrain_epochs` | `terrain_epochs() -> u32` | Tectonic epochs; default `12`, accepted `1..12`. |
| `terrain_erosion_steps` | `terrain_erosion_steps() -> u32` | Erosion steps; default `3`, accepted `0..3`. |
| `terrain_initial_climate_iterations` | `terrain_initial_climate_iterations() -> u32` | Initial climate iterations; default `24`, accepted `1..24`. |
| `terrain_climate_iterations` | `terrain_climate_iterations() -> u32` | Final climate iterations; default `80`, accepted `1..80`. |
| `terrain_currents` | `terrain_currents() -> bool` | Frozen currents flag; default `true`. |
| `terrain_wind_sign` | `terrain_wind_sign() -> i8` | Frozen wind sign; default `+1`, accepted `-1` or `+1`. |
| `terrain_coastal_elaboration` | `terrain_coastal_elaboration() -> bool` | Frozen coastal-elaboration flag; default `true`. |
| `terrain_plan` | `terrain_plan(u8, u64, u32, u32, u32, u32, u32, bool, i8, bool, bool, u32, u32, u32, u32, u8) -> u8` | Reads manifest version `2`, those nine frozen configuration fields, pre-generation, four contiguous extent slots, and a stage marker (`0` HeightTile, `3` Materialize, or `255` for no chunk stage). It is called once after each getter has been read once; its result echoes the marker and must match the captured plan. |

The captured configuration is validated before the plan is accepted; invalid
values fail preparation. The private manifest has a 4,096-byte source cap,
65,536-byte bytecode cap, 64 static instructions, ten host calls, and one
execution. These implementation limits and bindings do not extend to public
load-time world scripts above. The fixed private source reads each of the nine
configuration fields once, then calls `terrain_plan` with manifest version `2`,
pre-generation enabled, extents `4,096`, `1,024`, `256`, and `64`, and the
`Materialize` stage marker (`3`). These private configuration fields have no
public measurement-unit contract; malformed reads, unsupported versions,
invalid configuration or plan values, and a mismatched marker fail preparation.
