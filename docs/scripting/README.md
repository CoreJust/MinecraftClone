# Deterministic scenario scripts

Scenario sources are loaded once and lowered to an immutable authoritative
plan. They are not evaluated in a render frame or server tick. The source
header selects one of two explicit frontends:

- Legacy `.mcscenario` sources begin with `scenario 1`. They retain the
  line based `flat2d-v1`, `flat3d-v1`, and `flight3d-v1` profiles.
- CoreLang `.core` sources begin with `@version("0.0.3")`, use
  `@use minecraft`, and export `pub fn scenario()`. They currently target
  `flight3d-v1`.

An unknown or mixed header is rejected. Legacy syntax and flat profiles keep
their existing interpretation; CoreLang is a separate frontend, not a
renaming of `script 1`.

Use this guide as the entry point:

- [Quickstart](QUICKSTART.md) shows both a legacy and a CoreLang scenario.
- [Language reference](REFERENCE.md) defines grammar and typed host calls.
- [Execution model](EXECUTION.md) defines loading, authority, ordering, and
  resource limits.
- [Diagnostics](DIAGNOSTICS.md) lists stable failure codes.
- [Examples](../../scenarios/README.md) lists checked-in sources.

CoreLang uses the trusted packaged CoreLang 0.0.3 ruleset. It supports ordinary
CoreLang functions, conditions, and loops, so a scenario can compose reusable
operations and exercise multiple players. The Minecraft host exposes only the
typed calls `profile`, `seed`, `playerXYZ`, `moveXYZ`, `cameraInputXYZ`,
`wait`, and `expectXYZ`. Calls collect a private plan; publication happens only
after compilation and execution complete successfully.

The host bounds source bytes, statements, actors, total waited ticks,
operations, and expectation evidence. These are host configuration, not script
options. They bound accepted host operations and plan size, but do not promise
that arbitrary CoreLang control flow terminates: the ruleset has functions,
conditions, and loops, and this integration does not add a general instruction
fuel budget. Keep packaged scripts finite and use the host limits as resource
guards.

World generation is a separate load-time CoreLang source. The packaged
`scenarios/world/canonical_world.core` calls `terrain_random`, `set_block`, and
`publish` while building a private 16x16x16 candidate. A failed compile,
runtime call, or incomplete publication leaves the live world unchanged.
