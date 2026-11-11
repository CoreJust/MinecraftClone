# Scenario language reference

The bare `@version(0.1.5)` header selects the scenario frontend. Its
`minecraft` ruleset version 1 exposes two profiles: `flight3d-v1` for
authoritative multiplayer movement and `sparse-world-v1` for terrain
observations. Its registered host-call ABI major is 1. A source file must
export `scenario() -> unit`; every host call below also returns `unit`. Calls
have no implicit argument defaults. The host accepts calls only while lowering
this entrypoint, in source order.

```text
@version(0.1.5)
@use minecraft

pub fn scenario() {
    profile("flight3d-v1")
    seed(42u64)
    playerXYZ("Alice", '@', 32i32, 32i32, 8i32, 0i16, 0i16, 0i16)
    moveXYZ("Alice", 1i8, 0i8, 0i8)
    wait(1u64)
    expectXYZ("Alice", 33i32, 32i32, 8i32)
}
```

The `profile` and `seed` declarations are required exactly once. `profile`
must come first; `seed` follows it. Player declarations follow `seed`. Calls
that name a player require a declaration already collected for that name.
There is no default profile, seed, player, world-generator version, or
resident-chunk limit. Profile-specific calls and their ordering are listed
below. Any accepted host-call failure, invalid declaration, or missing
required declaration prevents plan publication and is reported as
`CoreLangRuntimeFailure`; see [Diagnostics](DIAGNOSTICS.md) for stable source
diagnostic names.

## `flight3d-v1`

| Call | Contract |
| --- | --- |
| `profile(str) -> unit` | Exactly once and first; accepts `"flight3d-v1"` or `"sparse-world-v1"`. |
| `seed(u64) -> unit` | Exactly once after `profile`; accepts the full unsigned 64-bit range. |
| `playerXYZ(str, c8, i32, i32, i32, i16, i16, i16) -> unit` | Flight only, after `seed`; name must match `[A-Za-z_][A-Za-z0-9_]*`, and both names and characters must be unique. Character is one of `@ # $ % &`. Orientation is yaw `0..359`, pitch `-89..89`, roll `-180..180` degrees. The `i32` position has no collector-side range check; its world-cell unit is inferred from the downstream `PlayerPosition` consumer. Flight spawn admission later checks world bounds and body/terrain fit; its z admission range is `0..65,535`, not the stale `-64..64` rule. The application limit is four actors. |
| `moveXYZ(str, i8, i8, i8) -> unit` | Flight only; existing actor; direct x/y/z direction components each `-1..1`. Input takes effect at the next tick boundary. |
| `flightXYZ(str, i8, i8, i8) -> unit` | Flight only; existing actor; each flight-intent component is `-1..1`. Applying it requires server-granted flight permission. |
| `phaseXYZ(str, i8, i8, i8) -> unit` | Flight only; existing actor; each phase-intent component is `-1..1`. Applying it requires server-granted collision-bypass permission. |
| `cameraInputXYZ(str, i8, i8, i8) -> unit` | Flight only; existing actor; strafe, forward, and vertical components each `-1..1`. The server resolves horizontal direction from the actor's yaw; camera pitch and roll do not affect movement. Input takes effect at the next tick boundary. |
| `movementPermissions(bool, bool) -> unit` | Flight only; arguments are `(flight, collision_bypass)`. Collision bypass requires flight. The operation publishes server-owned capabilities; valid states are both enabled, flight only, or both disabled. |
| `jump(str) -> unit` | Flight only; existing actor; queues one jump input and consumes one tick from the total tick budget. |
| `wait(u64) -> unit` | Flight only; value must be positive. Advances exactly that many authoritative ticks. The combined `wait` values and one tick per `jump` cannot exceed the host tick limit. |
| `expectXYZ(str, i32, i32, i32) -> unit` | Flight only; existing actor; records expected position at the current boundary. Position values use the downstream player-position coordinates; cell units are inferred from that consumer. Counts as one operation and one evidence item. |
| `expectMovementPermissions(str, bool, bool) -> unit` | Flight only; existing actor; expected `(flight, collision_bypass)` state, with collision bypass requiring flight. Checks replicated server capabilities; counts as one operation and one evidence item. |
| `expectVerticalVelocity(str, i32) -> unit` | Flight only; existing actor; expected replicated vertical velocity in subcells per tick, inferred from the runner's velocity representation. Counts as one operation and one evidence item. |

The three direction components are bounded for all four input calls:
`moveXYZ`, `flightXYZ`, `phaseXYZ`, and `cameraInputXYZ`. Each component is an
`i8`, but only `-1`, `0`, or `1` is accepted. `flightXYZ` and `phaseXYZ`
record intent; the authoritative server checks the corresponding granted
capability when the runner applies that input.

`playerXYZ` accepts all `i32` values during collection and does not enforce a
`-64..64` interval. On Flight join, spawn validation checks z against the
world's entity range (`0..65,535`), then checks the full position and body
against world and terrain constraints. The API has no default position or
orientation. The coordinate unit follows the world-cell fields consumed by
the server and is therefore an inference rather than a unit declared by the
host binding.

## `sparse-world-v1`

This profile has no players, movement permissions, jumps, or tick waits.
Configure the generator once after `profile` and `seed`, then observe blocks
or current residency:

```text
@version(0.1.5)
@use minecraft

pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 1u64)
    expectBlockXYZ(0i64, 0i64, 0i64, 1u8)
    expectResidentChunks(1u64)
}
```

| Call | Contract |
| --- | --- |
| `sparseWorldOptions(u32, u64) -> unit` | Sparse profile only; exactly once after `profile` and `seed`, before observations. Generator version must be `1`; resident-chunk bound must be positive and no greater than the configured host limit (default `128`). Counts as one operation. |
| `expectBlockXYZ(i64, i64, i64, u8) -> unit` | Requires `sparseWorldOptions`. Coordinates are world block coordinates; horizontal x/y wrap around the 65,536-block world and z must be within `0..1,023` when observed. Block ID is Air `0` or Stone `1`. Counts as one operation and one evidence item. |
| `expectResidentChunks(u64) -> unit` | Requires `sparseWorldOptions`; expected count must not exceed the selected resident-chunk bound. Checks current residency and counts as one operation and one evidence item. |

The sparse-world observations use the configured seed and the existing
`SparseWorld` and `TerrainGenerator`. They materialize blocks as needed while
maintaining the selected resident-chunk bound. See
[Execution](EXECUTION.md) for the limits applied while these calls are
collected and [World generation](WORLD_GENERATION.md) for the separate
load-time script APIs.
