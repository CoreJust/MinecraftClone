# Entity permissions

[Policy.hpp](../../src/shared/include/shared/policy/Policy.hpp) defines
server-owned policies for player, mob, and technical-entity subjects. A policy
source uses the CoreLang `minecraft` ruleset and exports
`pub fn policy() -> unit`. The ruleset is version 1 and its host-call ABI major
is 1. The adapter does not separately compare source-version headers; CoreLang
owns source syntax and version acceptance. The example below uses the packaged
CoreLang 0.1.5 syntax.

```corelang
@version(0.1.5)
@use minecraft

pub fn policy() {
    policyGroup("nearby", 5u8, "")
    policyMember("nearby", 42u64)
    policyRule("movement", "minecraft:speed", 200i64, 0u8)
    policyAssign("nearby", "movement", 0u8)
}
```

The five registered calls all return `unit`. They collect a private policy
plan; subject and capability names are resolved later by materialization.
String arguments must contain 1–256 printable ASCII bytes (`0x21`–`0x7e`),
except that `policyGroup`'s scope is empty for explicit groups. The adapter
does not give capability values universal units or ranges: the registered
capability definition supplies each key's default, inclusive minimum and
maximum, and hard-restriction direction. Capability keys must use
`<namespace>:<name>` with exactly one colon, nonempty parts, and lowercase
ASCII letters, digits, `.`, `_`, `-`, or `/`.

## Host calls

| Call | Accepted values and checks |
| --- | --- |
| `policyGroup(str, u8, str) -> unit` | `(name, kind, scope)`. `kind` is `4` EntityKind, `5` Explicit, or `6` Selector; values `0` All, `1` Players, `2` Mobs, and `3` TechnicalEntities are built-in kinds and cannot be declared as custom groups. Names are unique and cannot redefine `all`, `players`, `mobs`, or `entities`, use the reserved `world`/`world:` namespace, or collide with an `entity:<id>` or `kind:` target. EntityKind scope names an entity kind, Selector scope names a selector, and Explicit scope must be empty. Selector groups must refer to a selector by materialization. Maximum 64 groups. |
| `policyMember(str, u64) -> unit` | `(explicit_group, entity_id)`. The group must already have been declared with kind `5`; an entity ID may be listed once per group. IDs use the full unsigned 64-bit type range, then must resolve to a current subject during materialization. Maximum 256 members per group. |
| `policySelector(str, str, u64, u64, u64) -> unit` | `(name, entity_kind, first_id, last_id, max_results)`. Selector names are unique. IDs form an inclusive range and require `first_id <= last_id`; `max_results` must be positive. At materialization, the selector matches subjects with the exact entity-kind string and an ID in that range. Exceeding its result cap fails materialization. Maximum 64 selectors. |
| `policyRule(str, str, i64, u8) -> unit` | `(preset, capability_key, value, hard)`. The first rule creates the preset; later rules add distinct capability keys. `hard` accepts only `0` or `1`. The key must exist in the capability registry and the signed value must be within that definition's inclusive bounds at materialization. Maximum 64 presets and 64 distinct rules per preset. |
| `policyAssign(str, str, u8) -> unit` | `(target, preset, hard)`. `hard` accepts only `0` or `1`; the preset must already have been created by `policyRule`. Each target may be assigned once, and `world` and `world:` targets are reserved. Targets are resolved during materialization. Maximum 256 assignments. |

Group-kind discriminants are part of the host-call contract:

| Value | Kind | Meaning |
| ---: | --- | --- |
| `0` | All | Built-in group containing every subject |
| `1` | Players | Built-in player subjects |
| `2` | Mobs | Built-in mob subjects |
| `3` | TechnicalEntities | Built-in technical-entity subjects; this is what `entities` names |
| `4` | EntityKind | Custom group matching one entity-kind string |
| `5` | Explicit | Custom group containing listed current entity IDs |
| `6` | Selector | Custom group whose membership is resolved from a named selector |

Assignment targets may name built-in groups (`all`, `players`, `mobs`,
`entities`), `kind:<kind>`, `entity:<decimal-id>`, or a declared custom group.
Selectors and explicit memberships resolve against the current subject catalog;
an absent subject, unknown target, or selector result overflow prevents
materialization. Each subject starts with the registry default for every
capability. Soft rules use this target-specificity order; equal-specificity
ties choose the more restrictive value according to that capability's registry
definition:

| Specificity | Target |
| ---: | --- |
| 1 | `all` |
| 2 | `players`, `mobs`, or `entities` for the matching subject class |
| 3 | `kind:<kind>` or an EntityKind group |
| 4 | A Selector group |
| 5 | An Explicit group |
| 6 | `entity:<id>` for that exact subject |

Hard rules combine to the strongest constraint and can only tighten the result.
The built-in `entities` group contains technical entities, not all entity
kinds.

## Minecraft interaction adapter

`GameServer` looks up these exact action capability keys:
`minecraft:block.break`, `minecraft:block.place`, `minecraft:block.quick-break`,
`minecraft:flight`, `minecraft:collision-bypass`, and `minecraft:mode.change`.
The logical action is `mode.change`; its implemented registry key is namespaced.
Bare `mode.change` and `block.break` names do not match these lookups.
`minecraft:flight` and `minecraft:collision-bypass` are required when publishing
a server policy. Each required registry entry must have inclusive bounds `0..1`
and `Maximum` hard restriction; its default must lie in those bounds. The server
rejects publication if either definition is missing or does not meet those
constraints. Other action keys are optional; when present, a positive capability
value grants that soft action, while an absent key leaves the normal mode or bind
default in force. Mode changes default to allowed on loopback and denied for
remote clients unless `minecraft:mode.change` is registered.

Break and placement material filters use separate keys:
`minecraft:block.break.<material>` and `minecraft:block.place.<material>`, with
`<material>` equal to `air`, `stone`, `grass`, `dirt`, `bedrock`, `snow`, or
`ice`. Each registered key is evaluated independently; a nonpositive value
blacklists that material for its action, while an absent key adds no material
filter. The server adapter and its key list are in
[`GameServer.cpp`](../../src/server/GameServer.cpp).

## Limits and execution

These are `PolicyLimits` defaults. A host may supply another positive value;
zero in any limit is rejected as `invalid-limits`.

| Limit | Default |
| --- | ---: |
| Source bytes | 65,536 |
| Compiled bytecode bytes | 1,048,576 |
| Native image bytes | 4,194,304 |
| Static instruction bound | 16,384 |
| Policy host-call statements | 256 |
| Custom groups | 64 |
| Members per explicit group | 256 |
| Selectors | 64 |
| Presets | 64 |
| Rules per preset | 64 |
| Assignments | 256 |
| Subjects during materialization | 4,096 |
| Registered capabilities during materialization | 64 |

Source size is measured before parsing. The instruction bound covers module
initialization, `policy`, reachable helpers, and any requested interpreter
parity run. `PolicyCompileOptions` defaults to the Interpreter backend with
interpreter parity enabled; that default executes once. A non-Interpreter
backend with parity enabled executes the selected backend and interpreter, so
the static instruction budget covers two runs. All custom limit values must be
positive. Finite `for` loops are accepted only with matching, nonnegative
fixed-width integer literal endpoints and an immutable named index; index
writes, aliases, captures, and address escapes are rejected. Dynamic loops,
source imports, indirect calls, recursion, and bytecode static-init
instructions are rejected. This preflight bound is not runtime instruction
fuel. Cancellation is checked during bounded analysis and compilation, around
backend phases, and at host-call boundaries; a running runtime or native call
does not receive instruction-by-instruction cancellation.

`PolicyHost::compile` returns a collected plan only after CoreLang compilation,
bounded execution of `policy`, and validation of its host calls. A policy must
make at least one host call. `materialize` checks catalog validity, resolves
selectors and targets, validates capability values, and creates immutable
dense rows for the current subjects. Publication requires a snapshot
materialized for that exact plan; publishing without one reports
`missing-materialization`, and publishing a snapshot for another plan reports
`materialization-mismatch`. Compilation, materialization, and publication are
separate operations and should complete outside physics ticks.

## Stable diagnostics

`policyDiagnosticCodeName` returns these machine-readable names. The
human-readable message may contain source or backend details.

| Code | Name | Meaning |
| --- | --- | --- |
| `InvalidLimits` | `invalid-limits` | At least one configured policy limit is zero. |
| `SourceTooLarge` | `source-too-large` | Source exceeds the configured byte limit. |
| `StatementLimitExceeded` | `statement-limit-exceeded` | Policy host-call statements exceed the limit. |
| `GroupLimitExceeded` | `group-limit-exceeded` | Custom groups exceed the limit. |
| `MemberLimitExceeded` | `member-limit-exceeded` | Explicit-group members exceed the per-group limit. |
| `SelectorLimitExceeded` | `selector-limit-exceeded` | Selector count or materialized selector results exceed a limit. |
| `PresetLimitExceeded` | `preset-limit-exceeded` | Preset count exceeds the limit. |
| `RuleLimitExceeded` | `rule-limit-exceeded` | Rules in a preset exceed the limit. |
| `AssignmentLimitExceeded` | `assignment-limit-exceeded` | Assignments exceed the limit. |
| `InvalidDeclaration` | `invalid-declaration` | A declaration, limit, capability registry, or target value is invalid. |
| `DuplicateDeclaration` | `duplicate-declaration` | A materialized declaration name is duplicated. |
| `UnknownDeclaration` | `unknown-declaration` | A referenced group, selector, preset, subject, target, or capability is unknown. |
| `CompileFailure` | `compile-failure` | CoreLang parsing, type checking, or ruleset resolution failed. |
| `RuntimeFailure` | `runtime-failure` | The policy entrypoint or a registered host call failed. |
| `JitPreparationFailure` | `jit-preparation-failure` | JIT preparation failed. |
| `AotPreparationFailure` | `aot-preparation-failure` | AOT preparation failed. |
| `BytecodeLimitExceeded` | `bytecode-limit-exceeded` | Compiled bytecode exceeded its limit. |
| `NativeImageLimitExceeded` | `native-image-limit-exceeded` | The compiled native image exceeded its limit. |
| `InterpreterParityFailure` | `interpreter-parity-failure` | Selected-backend and interpreter plans differed. |
| `Cancelled` | `cancelled` | Cancellation was observed during a cancellable phase. |
| `InstructionLimitExceeded` | `instruction-limit-exceeded` | Static instruction analysis exceeded its bound. |
| `UnboundedExecution` | `unbounded-execution` | The source contains execution outside the bounded subset. |
| `MaterializationMismatch` | `materialization-mismatch` | The supplied capability snapshot belongs to another plan. |
| `MissingMaterialization` | `missing-materialization` | Publication was requested without a materialized snapshot. |

The server validates movement values and collision geometry before publishing,
then applies and replicates changed player capabilities. Disconnect
publication removes a departed ID from explicit groups and entity-targeted
assignments, so a later reused ID cannot inherit those concrete memberships or
overrides. See [Policy.hpp](../../src/shared/include/shared/policy/Policy.hpp)
and [CoreLangPolicy.cpp](../../src/shared/policy/CoreLangPolicy.cpp) for the
owning API and implementation.
