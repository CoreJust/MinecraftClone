# Scenario diagnostics

`parseScenarioSource` is the CoreLang source path. It returns stable
`ScenarioDiagnosticCode` names; human-readable messages may include a filename,
source location, or CoreLang backend detail.

| Code | Name | Meaning |
| --- | --- | --- |
| `InvalidLimits` | `invalid-limits` | A scenario or execution limit is zero. |
| `SourceTooLarge` | `source-too-large` | Source bytes exceed the configured host limit. |
| `UnknownSourceHeader` | `unknown-source-header` | The header is not the bare `@version(0.1.5)` form; the diagnostic expects `@version(0.1.5)`. |
| `CoreLangCompileFailure` | `corelang-compile-failure` | CoreLang parsing, type checking, ruleset resolution, or compilation failed. |
| `CoreLangRuntimeFailure` | `corelang-runtime-failure` | Static instruction validation, entrypoint execution, a typed host call, a host range or budget check, required declarations, runtime loading, or completion failed. Scenario actor, statement, tick, operation, and evidence-limit rejections on this CoreLang path use this code. |
| `Cancelled` | `cancelled` | Cancellation was observed before a plan was returned. |

Every failure above occurs before a scenario plan is published. A failed
host-call or required-declaration check discards any private candidate plan.
Expectation mismatches are execution evidence after a valid plan has begun;
they stop successful completion at the observed boundary and do not change the
diagnostic code.

The shared enum also retains names for the separate legacy `parseScenario`
text parser. Those codes are not used to report CoreLang host-call failures:
`statement-limit-exceeded`, `actor-limit-exceeded`, `tick-limit-exceeded`,
`operation-limit-exceeded`, `evidence-limit-exceeded`, `malformed-syntax`,
`unsupported-version`, `unsupported-profile`, `duplicate-actor`,
`unknown-actor`, `unsupported-command`, `integer-overflow`, `invalid-integer`,
`invalid-range`, `invalid-character`, and `missing-player`. In that legacy path,
`tick-limit-exceeded` counts waits plus one tick per jump.

World loading has a separate `ScriptedWorldErrorCode` family. It reports
invalid options, source overflow, compilation/runtime failure, host-call limit
exhaustion, or incomplete publication; those errors likewise discard the
private chunk candidate. Failures from bounded instruction preflight use the
existing `RuntimeFailed` value; there is no dedicated world-script budget
diagnostic. `TerrainGenerator` exposes construction failures as
`std::runtime_error`. See [World generation](WORLD_GENERATION.md) for the
complete callable contract, static execution bounds, and remaining runtime
trust boundary.
