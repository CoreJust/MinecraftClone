# Scenario examples

Checked-in sources are complete examples for the frontend selected by their
header. See the [scripting guide](../docs/scripting/README.md) for the
contracts.

Legacy `.mcscenario` examples preserve the original line based syntax and flat
profiles:

- `canonical_sample.mcscenario` is the compact `scenario 1` quickstart.
- `comment_and_boundary.mcscenario` covers comments, blank lines, `"#"`, and
  a boundary-zero expectation.
- `two_players.mcscenario` covers ordered input and separate actors.
- `camera_two_client.mcscenario` exercises the legacy flat 3D camera replay.

CoreLang `.core` examples use `@version("0.0.3")`, `@use minecraft`, and
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
