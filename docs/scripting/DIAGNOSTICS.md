# Scenario diagnostics

Diagnostics identify invalid source, invalid trusted limits, or failed
CoreLang lowering. The stable machine-readable name comes from
`scenarioDiagnosticCodeName`; human text may include filename and source
location.

| Code | Name | Meaning |
| --- | --- | --- |
| `InvalidLimits` | `invalid-limits` | A scenario limit is zero. |
| `SourceTooLarge` | `source-too-large` | Source bytes exceed the host limit. |
| `StatementLimitExceeded` | `statement-limit-exceeded` | Accepted host-call statements exceed the statement limit. |
| `ActorLimitExceeded` | `actor-limit-exceeded` | Player declarations exceed the actor limit. |
| `TickLimitExceeded` | `tick-limit-exceeded` | Sum of `wait` ticks exceeds the tick limit. |
| `OperationLimitExceeded` | `operation-limit-exceeded` | Emitted input, wait, and expectation operations exceed the operation limit. |
| `EvidenceLimitExceeded` | `evidence-limit-exceeded` | Expectations exceed the evidence limit. |
| `UnknownSourceHeader` | `unknown-source-header` | The source does not begin with CoreLang `@version("0.1.2")`. |
| `CoreLangCompileFailure` | `corelang-compile-failure` | CoreLang parsing, type checking, or ruleset resolution failed. |
| `CoreLangRuntimeFailure` | `corelang-runtime-failure` | A typed host call, required declaration, runtime load, or completion failed. |
| `Cancelled` | `cancelled` | Cancellation was observed before a plan was returned. |

All failures above occur before a scenario plan is published. The CoreLang
collector may have accumulated a private candidate when a later call fails;
that candidate is discarded. An expectation mismatch is execution evidence
after a valid plan has begun, and stops successful scenario completion at the
observed boundary rather than changing the diagnostic code.

World loading has a separate `ScriptedWorldErrorCode` family. It reports
invalid options, source overflow, compilation/runtime failure, host-call limit
exhaustion, or incomplete publication; those errors likewise discard the
private chunk candidate.
