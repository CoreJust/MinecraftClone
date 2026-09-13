# Scenario execution model

## Load and publication

`parseScenarioSource` checks that all six scenario limits are positive, checks
source bytes, observes an already requested cancellation, then selects the
explicit header. Legacy text is parsed directly. CoreLang is compiled with the
trusted `minecraft` ruleset, loaded into a runtime, and executes `scenario`.
Host calls append to a private `ScenarioPlanCollector`. Compilation, runtime,
host-call validation, and the final required declarations must all succeed
before an immutable plan is returned. A failure cannot mutate the live world,
network state, or authoritative runner.

The scenario runner then creates boundary-zero state, with declared players and
stopped input. Commands are ordered by source position. An input recorded at
boundary `B` is effective on the next authoritative step (`B + 1`) and remains
active until replaced. `wait(N)` advances exactly `N` fixed authoritative
steps and moves the boundary forward by `N`. `expect` records an observation at
the current boundary and does not advance time. Camera input is converted from
the actor's yaw into the authoritative XYZ direction; the server remains the
authority. Replay identity excludes wall-clock duration.

## Host bounds

The application scenario command currently supplies these limits:

| Limit | Value | Applies to |
| --- | ---: | --- |
| Source bytes | 65,536 | Entire source before parsing |
| Statements | 256 | Every accepted host-call statement |
| Actors | 4 | `playerXYZ` declarations |
| Total ticks | 10,000 | Sum of all `wait` values |
| Operations | 512 | Emitted inputs, waits, and expectations |
| Evidence | 128 | `expectXYZ` operations |

The API accepts other positive host configurations. Script input cannot raise,
lower, or replace them. CoreLang functions, conditions, and loops can execute
before or between host calls; these bounds limit accepted source and emitted
scenario work, but they are not a general instruction fuel guarantee for
arbitrary CoreLang control flow. The command wrapper also has a 30 second
runtime watchdog; that is an application deadline, not a language termination
proof.

## Cancellation and errors

Cancellation is checked before frontend selection and at each registered
CoreLang host call. If it is observed, lowering returns `cancelled` and no plan
is published. Cancellation is not a separate asynchronous interrupt supplied
to arbitrary CoreLang instructions, so callers must keep packaged scripts
bounded and finite.

Invalid limits, source size, compilation, typed host arguments, ranges,
duplicate actors, operation budgets, tick budgets, evidence budgets, and final
required declarations fail before publication. The legacy parser also validates
the complete source before setup. See [Diagnostics](DIAGNOSTICS.md) for stable
codes.

## Seeded world loading

World generation uses a separate load-time CoreLang program and private
candidate. `canonical_world.core` iterates a 16x16x16 chunk, calls the trusted
seeded `terrain_random`, writes only stone block id `1` where
`z - 6 + random < 0`, and calls `publish`. The world loader rejects invalid
options, source overflow, compilation/runtime failure, host-call overflow, or
incomplete publication; the live world is changed only by a completed chunk
and its resulting configuration identity.
