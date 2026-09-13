# Scenario diagnostics

Diagnostics identify invalid source or invalid trusted host configuration.
Their stable code and machine-readable name are part of the parser interface;
human wording may add filename, line, column, and context without changing the
code.

| Code | Machine-readable name | Meaning |
| --- | --- | --- |
| `InvalidLimits` | `invalid-limits` | At least one trusted host limit is not positive. |
| `SourceTooLarge` | `source-too-large` | Source bytes exceed the configured source limit. |
| `StatementLimitExceeded` | `statement-limit-exceeded` | Nonblank directive lines exceed the statement budget. |
| `ActorLimitExceeded` | `actor-limit-exceeded` | Player declarations exceed the actor budget. |
| `TickLimitExceeded` | `tick-limit-exceeded` | Requested `wait` ticks exceed the tick budget. |
| `OperationLimitExceeded` | `operation-limit-exceeded` | Emitted `input`, `wait`, and `expect` plan operations exceed the operation budget. |
| `EvidenceLimitExceeded` | `evidence-limit-exceeded` | `expect` operations exceed the evidence budget. |
| `MalformedSyntax` | `malformed-syntax` | The finite grammar, strict order, tokenization, or EOF rule is violated. |
| `UnsupportedVersion` | `unsupported-version` | The `scenario` version is not supported. |
| `UnsupportedProfile` | `unsupported-profile` | The selected profile is not supported for this format. |
| `DuplicateActor` | `duplicate-actor` | A player identifier or character is declared more than once. |
| `UnknownActor` | `unknown-actor` | A command references a player that was not declared. |
| `UnsupportedCommand` | `unsupported-command` | A syntactically command-like form is outside the supported command set. |
| `IntegerOverflow` | `integer-overflow` | A base-10 integer cannot be represented by its target field. |
| `InvalidInteger` | `invalid-integer` | A numeric token is not a permitted base-10 integer for its field. |
| `InvalidRange` | `invalid-range` | A profile field, direction, coordinate, or count is outside its allowed range. |
| `InvalidCharacter` | `invalid-character` | A character string is malformed or not supported by the selected profile. |
| `UnknownSourceHeader` | `unknown-source-header` | The first source header selects neither `scenario 1` nor CoreLang `@version("0.0.1")`. |
| `CoreLangCompileFailure` | `corelang-compile-failure` | CoreLang parsing, graph validation, or fixed-ruleset resolution failed before a plan exists. |
| `CoreLangRuntimeFailure` | `corelang-runtime-failure` | A bounded CoreLang callback rejected its typed scenario operation before a plan exists. |
| `Cancelled` | `cancelled` | Scenario lowering observed cancellation before returning a plan. |
| `MissingPlayer` | `missing-player` | The required one-or-more player declarations are absent. |

Parse and validation failures have no scenario side effect. A runner may report
an expectation mismatch as execution evidence after a fully validated plan has
begun; it is distinct from these parser diagnostic codes and terminates
successful scenario completion at the boundary where it is observed.
