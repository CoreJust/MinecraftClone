# Deterministic scenario scripts

Scenario scripts describe a small, deterministic authoritative simulation. They
are plain UTF-8 text files with two explicit frontends for the same
`flat2d-v1` plan: legacy `.mcscenario` sources start with `scenario 1`, while
CoreLang `.core` sources start with `@version("0.0.1")`. Shared selects only by
that header; an unknown or mixed header is rejected rather than guessed.

Use this guide as the entry point:

- [Quickstart](QUICKSTART.md) writes and reads a first scenario.
- [Language reference](REFERENCE.md) defines the complete grammar, commands,
  lexical rules, and profile contract.
- [Execution model](EXECUTION.md) defines timing, source order, authority,
  limits, and compatibility.
- [Diagnostics](DIAGNOSTICS.md) lists stable parser and validation failures.
- [Examples](../../scenarios/README.md) contains both canonical source forms.

Both frontends are deliberately finite scenario data, not game automation. A
CoreLang ruleset exposes only `profile`, `seed`, `player`, `input`, `wait`, and
`expect_position`; its callbacks collect a private plan before the existing
authoritative runner is allowed to start. There is no name injection, function
or loop support, VM, direct world/network/server effect, or legacy `script 1`
relabeling.

## Current support

`flat2d-v1` has a 32 by 32 board. It accepts characters `@`, `#`, `$`, `%`, and
`&`; coordinates are inclusive in `0..31`; direction components are in
`-1..1`; and every `wait` count is positive. Commands are limited to `input`,
`wait`, and `expect player ... position`.

The source is fully compiled/validated and lowered before it changes world
state. A successful script is deterministic for its profile, seed, source
order, and host limits.
