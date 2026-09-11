# Scenario examples

Every checked-in source in this directory is complete and parseable by the
header-selected frontend documented in [the scripting guide](../docs/scripting/README.md).

- [canonical_sample.mcscenario](canonical_sample.mcscenario) is the exact
  canonical quickstart source.
- [canonical_sample.core](canonical_sample.core) is the CoreLang equivalent;
  it lowers to the same five-tick authoritative plan.
- [comment_and_boundary.mcscenario](comment_and_boundary.mcscenario) shows
  blank lines, trailing comments, `"#"` as a character, and an expectation at
  boundary zero.
- [two_players.mcscenario](two_players.mcscenario) shows ordered input and
  expectations for separate actors.
- [camera_two_client.mcscenario](camera_two_client.mcscenario) is the bounded
  `flat3d-v1` replay: two camera poses map forward input to the unchanged
  authoritative cardinal direction wire, with Z explicitly fixed at zero.

The CoreLang sample uses `flat2d-v1`; `flat3d-v1` is currently a legacy-source
camera/replay profile, not a compiler, VM, or JIT feature.
