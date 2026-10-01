# Scenario language reference

The `minecraft` ruleset has two versioned scenario frontends: `0.1.2` for
`flight3d-v1` multiplayer scenarios and `0.1.3` for bounded `sparse-world-v1`
observations.

The source must have this shape:

```text
@version("0.1.2")
@use minecraft

pub fn scenario() { /* typed host calls and CoreLang control flow */ }
```

The scenario profile must be `flight3d-v1`. The registered host operations are:

| Call | Signature and contract |
| --- | --- |
| `profile` | `profile(str)` exactly once, with `"flight3d-v1"` |
| `seed` | `seed(u64)` once, after `profile` |
| `playerXYZ` | `playerXYZ(str, c8, i32, i32, i32, i16, i16, i16)` |
| `moveXYZ` | `moveXYZ(str, i8, i8, i8)`; components `-1..1` |
| `flightXYZ` | `flightXYZ(str, i8, i8, i8)`; requires server-granted flight permission |
| `phaseXYZ` | `phaseXYZ(str, i8, i8, i8)`; requires server-granted collision-bypass permission |
| `cameraInputXYZ` | `cameraInputXYZ(str, i8, i8, i8)`; components `-1..1` |
| `movementPermissions` | `movementPermissions(bool, bool)`; publishes server-owned flight and collision-bypass permissions |
| `jump` | `jump(str)`; sends one jump input tick when flight is disabled |
| `wait` | `wait(u64)`; value must be positive |
| `expectXYZ` | `expectXYZ(str, i32, i32, i32)` |
| `expectMovementPermissions` | `expectMovementPermissions(str, bool, bool)`; checks replicated capabilities |
| `expectVerticalVelocity` | `expectVerticalVelocity(str, i32)`; checks replicated velocity in subcells per tick |

`playerXYZ` accepts characters `@ # $ % &`, unique ASCII names and characters,
positions in `-64..64`, yaw `0..359`, pitch `-89..89`, and roll `-180..180`.
Player declarations follow `seed`. `moveXYZ` submits authoritative XYZ input;
`cameraInputXYZ` submits strafe, forward, and vertical components and is
resolved from the player's yaw. A camera operation's pitch and roll do not
change movement.

`movementPermissions` is applied by the scenario server; clients cannot grant
capabilities through input packets. The valid combinations are both enabled,
flight only, or both disabled. `flightXYZ` and `phaseXYZ` check the corresponding
published capability before sending input. `jump` lasts one tick, and the
physics expectations observe authoritative replicated state.

Functions, `if`, and `for` are supported by CoreLang and are useful for
reusable scenario composition. There is no generic Minecraft instruction fuel
or arbitrary host access in this integration. Keep control flow finite.

## Sparse-world scenario (`0.1.3`)

This profile has no players or tick operations. Configure the generator and
resident-chunk bound before observing generated blocks or current residency:

```text
@version("0.1.3")
@use minecraft

pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 1u64)
    expectBlockXYZ(0i64, 0i64, 0i64, 1u8)
    expectResidentChunks(1u64)
}
```

| Call | Signature and contract |
| --- | --- |
| `sparseWorldOptions` | `sparseWorldOptions(u32, u64)`; selects generator version `1` and a positive resident-chunk limit no greater than the host limit (default `128`) |
| `expectBlockXYZ` | `expectBlockXYZ(i64, i64, i64, u8)`; materializes and checks Air (`0`) or Stone (`1`) at a world coordinate |
| `expectResidentChunks` | `expectResidentChunks(u64)`; checks current resident-chunk count, which cannot exceed the configured limit |

Each observation consumes one operation and one evidence slot. The host also
bounds source bytes, statements, total operations, and evidence. Distant
observations use the existing `SparseWorld` and `TerrainGenerator`; traversal
evicts chunks under the configured residency limit. Unsupported generator
versions, block IDs, or bounds are rejected before a plan is published.

## World source

`scenarios/world/canonical_world.core` exports `generate(u64)`. Its trusted
world host calls are `terrain_random(seed, x, y, z)`, `set_block(x, y, z, id)`,
and `publish()`. The formula is `z - 6 + random < 0`; it fills a private
16x16x16 candidate with stone block id `1`, then publishes once.
