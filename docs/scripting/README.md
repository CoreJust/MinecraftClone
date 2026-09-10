# Deterministic scenario scripts

Scenario scripts describe a small, deterministic authoritative simulation. They
are plain UTF-8 text files, conventionally named `.mcscenario`, and currently
support one profile: `flat2d-v1`. This is the initial versioned scripting layer:
source is parsed into typed operations that the authoritative host controls.

Use this guide as the entry point:

- [Quickstart](QUICKSTART.md) writes and reads a first scenario.
- [Language reference](REFERENCE.md) defines the complete grammar, commands,
  lexical rules, and profile contract.
- [Execution model](EXECUTION.md) defines timing, source order, authority,
  limits, and compatibility.
- [Diagnostics](DIAGNOSTICS.md) lists stable parser and validation failures.
- [Examples](../../scenarios/README.md) contains parseable `.mcscenario`
  sources, including the canonical sample.

Scripts are deliberately finite data, not a general-purpose programming
language. They construct a small authoritative world, submit inputs, advance a
known number of simulation steps, and inspect authoritative state. See the
[security model](EXECUTION.md#authority-and-safety) before treating a script as
an automation boundary.

## Current support

`flat2d-v1` has a 32 by 32 board. It accepts characters `@`, `#`, `$`, `%`, and
`&`; coordinates are inclusive in `0..31`; direction components are in
`-1..1`; and every `wait` count is positive. Commands are limited to `input`,
`wait`, and `expect player ... position`.

The source is parsed and validated before it changes world state. A successful
script is deterministic for its profile, seed, source order, and host limits.
