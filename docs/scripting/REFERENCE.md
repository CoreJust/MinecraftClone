# Scenario language reference

CoreLang 0.1.2 is the only scenario frontend. It uses the pinned `minecraft`
ruleset.

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
| `cameraInputXYZ` | `cameraInputXYZ(str, i8, i8, i8)`; components `-1..1` |
| `wait` | `wait(u64)`; value must be positive |
| `expectXYZ` | `expectXYZ(str, i32, i32, i32)` |

`playerXYZ` accepts characters `@ # $ % &`, unique ASCII names and characters,
positions in `-64..64`, yaw `0..359`, pitch `-89..89`, and roll `-180..180`.
Player declarations follow `seed`. `moveXYZ` submits authoritative XYZ input;
`cameraInputXYZ` submits strafe, forward, and vertical components and is
resolved from the player's yaw. A camera operation's pitch and roll do not
change movement.

Functions, `if`, and `for` are supported by CoreLang and are useful for
reusable scenario composition. There is no generic Minecraft instruction fuel
or arbitrary host access in this integration. Keep control flow finite.

## World source

`scenarios/world/canonical_world.core` exports `generate(u64)`. Its trusted
world host calls are `terrain_random(seed, x, y, z)`, `set_block(x, y, z, id)`,
and `publish()`. The formula is `z - 6 + random < 0`; it fills a private
16x16x16 candidate with stone block id `1`, then publishes once.
