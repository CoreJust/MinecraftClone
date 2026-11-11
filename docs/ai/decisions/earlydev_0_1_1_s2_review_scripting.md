# S2 W9 — CoreLang API contracts and bounded execution

Planning only, 23 October 2026. The frozen S1 review is evidence for S2 work;
it does not establish that any finding is fixed. The reviewed source snapshot
has manifest SHA-256 `ee51ffbf382740c701b3226bf9c8fda385dd6668456b757bec8eef4689b93307`.
The committed [API inventory](earlydev_0_1_1_s2_review_api.json) and
[review register](earlydev_0_1_1_s2_review.md) retain the source-bound details.

## Scope and boundaries

W9 makes the callable CoreLang contracts accurate and gives public world-script
loading a demonstrable execution boundary. The inventory covers 29 public host
bindings (16 scenario, 5 policy, 3 scripted-world, 5 world-generation-plan),
10 private terrain-v2 manifest bindings, three world-generation parameter
exports, and the three-function generic C++ bounded-execution bridge. Preserve
all eleven coverage fields for each of the 39 bindings: name, argument types,
return type, profile, entrypoint, units, defaults, limits, order, errors, and
version. Publish complete contracts for public bindings, while keeping the ten
terrain-v2 calls in clearly marked internal documentation; do not present them
as a user script API. Mark a field not applicable only when that is accurate.

W9 follows reconciliation with the final S1 source and pinned CoreLang revision.
W1 remains the authority for shared policy and the `PolicyCapabilityRegistry`;
S2 mode rights use that generic registry and `PolicyHost` materialization, not a
new scenario-only channel. W8 owns the selected terrain-model integration and
generation consumer. W9 documents those contracts without selecting a model or
expanding the upstream CoreLang API.

## Work and acceptance

1. **Scenario reference, execution, and diagnostics (API-001–004, 007).** In
   `docs/scripting/REFERENCE.md`, scope calls to the two supported profiles and
   state the shared `-1..1` component range for `moveXYZ`, `flightXYZ`,
   `phaseXYZ`, and `cameraInputXYZ`. Preserve API-002's actual behavior: the
   collector accepts `i32` x/y/z without a `-64..64` check; later Flight spawn
   admission checks z against world bounds, not that stale interval or x/y.
   Document the actual rejection point; trace units through the world consumer
   and label genuine inference. In `EXECUTION.md`, define tick accounting as waits plus one per
   jump, and specify operation/evidence counts by their complete collector
   semantics. In `DIAGNOSTICS.md`, make the expected source header the accepted
   bare `@version(0.1.5)`. Acceptance: both minimal profiles parse; coordinate
   behavior, each budget boundary/next-item rejection, all four input axes at
   -2 through 2, and the stale-header diagnostic match docs and source.

2. **Policy and world-loading references (API-005–006).** Add typed call
   tables and usable examples to `docs/code/POLICY.md` for all five policy
   bindings, including kind discriminants, `u8` flags, ordering, limits,
   materialization failures, and stable diagnostic names. State that capability
   units/ranges come from the registry. Add separate callable-contract tables
   for the three scripted-world calls and five generation-plan calls in the
   scripting guides; include argument and return types, entrypoints, profiles,
   defaults, bounds, order, errors, and version. Document the three parameter
   getter exports with their defaults and validation, label inferred block
   units as inference, and list both packaged world sources in
   `scenarios/README.md`. Acceptance: positive and negative contract coverage
   for each policy binding; generation examples or negative fixtures cover each
   invalid ordering/value in the inventory, reviewed against registrations.

3. **Bounded execution (API-008).** Before caller- or editor-authored source is
   accepted, route both `ScriptedWorld::load` and generation-plan/getter
   execution through bounded validation/execution with a static instruction
   proof and cancellation, using the existing shared bounded bridge where its
   contract fits. The pinned ordinary loader runtime has no instruction/fuel
   limit and no loader-owned cancellation handle, so callback/source caps alone
   do not pass. If a loader is instead restricted to trusted finite packaged
   source, enforce that boundary at the call site and test rejection of other
   sources. Acceptance: finite, over-budget, and nonterminating inputs either
   fail before execution or cancel within a deterministic deadline; tests prove
   the trusted-source restriction if that route is chosen. Do not claim this
   finding fixed before those results exist.

After source reconciliation, update the owning guides and examples, then run
their focused contract tests and `python3 script/ai_docs.py refresh` followed
by `check`. This plan adds no test, build, or source result and does not close
S2 acceptance.
