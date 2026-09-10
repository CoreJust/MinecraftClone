# Scenario examples

Every `.mcscenario` file in this directory is a complete, parseable source for
the format documented in [the scripting guide](../docs/scripting/README.md).

- [canonical_sample.mcscenario](canonical_sample.mcscenario) is the exact
  canonical quickstart source.
- [comment_and_boundary.mcscenario](comment_and_boundary.mcscenario) shows
  blank lines, trailing comments, `"#"` as a character, and an expectation at
  boundary zero.
- [two_players.mcscenario](two_players.mcscenario) shows ordered input and
  expectations for separate actors.

Examples use only `flat2d-v1`; they do not demonstrate future profiles,
compiler, VM, or JIT features because those are not supported today.
