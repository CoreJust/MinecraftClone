# Scenario language reference

There are two source grammars. `scenario 1` is the legacy line based grammar;
its declarations and flat profiles remain compatible. CoreLang uses the pinned
0.0.3 compiler and the `minecraft` ruleset.

## Legacy frontend

The order is strict: `scenario 1`, `profile`, `seed`, one or more `player`
declarations, `begin`, zero or more commands, `end`, then EOF. Blank lines and
comments are accepted. `flat2d-v1` uses coordinates `0..31`; `flat3d-v1` keeps
the flat board and requires `z == 0`; `flight3d-v1` uses signed XYZ positions
in `-64..64`. Player characters are one of `@ # $ % &`, names are ASCII
identifiers, and names and characters are unique.

Legacy flight commands are:

```text
input PLAYER X Y Z
input PLAYER camera STRAFE FORWARD VERTICAL
wait POSITIVE_INTEGER
expect player PLAYER position X Y Z
```

Every input component is in `-1..1`. Flight orientation is supplied on the
player declaration: yaw `0..359`, pitch `-89..89`, roll `-180..180`. Camera
input is resolved using yaw; pitch and roll are replay metadata. The existing
flat commands and their two dimensional forms remain available in their
profiles.

## CoreLang scenario frontend

The source must have this shape:

```text
@version("0.0.3")
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
