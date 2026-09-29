# Scenario examples

Checked-in sources are CoreLang scripts. See the
[scripting guide](../docs/scripting/README.md) for the contracts.

CoreLang `.core` examples use `@version("0.1.2")`, `@use minecraft`, and
`pub fn scenario()`:

- `canonical_sample.core` is the smallest typed flight scenario.
- `s5_flight_boundary.core` uses functions and loops to move two players to
  signed XYZ boundaries and back.
- `s5_flight_camera.core` exercises typed camera input, yaw resolution, and
  vertical movement.
- `s5_flight_multiplayer.core` combines reusable functions, a condition,
  direct XYZ input, camera input, and two players.
- `s5_flight_invalid.core` is a negative fixture: duplicate characters must be
  rejected before a plan is published.

`world/canonical_world.core` is loaded separately at world-load time. Its
`generate(seed)` function samples a deterministic 16x16x16 candidate using
`z - 6 + random < 0`, writes stone block id `1`, and publishes only after the
candidate is complete. It is seeded world content, not a per-tick scenario.

The `sparse-world-v1` profile uses the versioned `0.1.3` contract.
`sparseWorldOptions` selects generator version `1` and a positive resident
chunk bound. `expectBlockXYZ` materializes and checks a block through the
existing sparse world; `expectResidentChunks` checks current residency. Each
observation counts against the scenario operation and evidence limits.

```corelang
@version("0.1.3")
@use minecraft

pub fn scenario() {
    profile("sparse-world-v1")
    seed(42u64)
    sparseWorldOptions(1u32, 1u64)
    expectBlockXYZ(0i64, 0i64, 0i64, 1u8)
    expectResidentChunks(1u64)
    expectBlockXYZ(32768i64, 32768i64, 10i64, 1u8)
    expectResidentChunks(1u64)
    expectBlockXYZ(65536i64, 65536i64, 10i64, 0u8)
    expectResidentChunks(1u64)
}
```
