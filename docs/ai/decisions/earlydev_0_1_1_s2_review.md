# S2 — Broad S1 review follow-up

Planning only, 23 October 2026; [MC-AI-0524](../tasks/MC-AI-0524.md) records the
owner's instruction to add the completed review to S2. This extends the
[accepted S2 plan](earlydev_0_1_1_s2.md) and
[S1 carryovers](earlydev_0_1_1_s2_delivery_targets.md); it does not replace them.

## Evidence and closure rules

The audit identified 28 distinct items: 17 source-confirmed issues/documentation
gaps, four architectural/measurement constraints and seven existing S2
prerequisites. Some issues were already known. ARC-004 duplicates GPU-03.
The [portable register](earlydev_0_1_1_s2_review_findings.json) preserves each
classification, source location, consequence, remedy, acceptance and owner.
Line references describe the audited working source, not the planning checkout.

The reviewed S1 source was based on `7b3f89e1f118fa3013c403f53658ebe92be4eb07`,
including uncommitted fixes and the recorded configuration/test deltas. Its
manifest identities and exact CoreLang pin are in the register. No native
tests, device runs or performance benchmarks were executed by the audit;
later test-only cleanup was excluded. Reconcile every item against final S1
before implementation; source-matched proof that S1 already fixed it is a
valid disposition.

Confirmed issues require fixes and their recorded acceptance, or evidence they
no longer apply. Existing prerequisites close through their original S2
contracts. Measurement constraints require a reproducible decision: retaining
the current implementation is valid when evidence supports it or remains
inconclusive. Do not invent speedups, a fastest hash, mandatory SIMD/GPU work,
or a requirement to saturate every core. P3 findings remain lower-priority
S2 work, not silently omitted items. No item is marked fixed here.

## Complete disposition map

| Review ID | S2 work | Package |
|---|---|---|
| SIM-001 | Durable sparse edits independent of dense residency. | W2 |
| SIM-002 | Revisioned updates for clients with resident chunks. | W4 |
| SIM-003 | Edited-column manifests and full voxel coverage. | W4 |
| SIM-004 | Separate modes/action rights from movement bits. | W1/W3 |
| TER-01 | Remove tile-wide dense reconstruction from untouched column reads. | W0/W2 |
| CPU-001 | Measure combined local pools before changing CPU budgets. | W0 |
| CPU-002 | Skip unconsumed progress snapshots; preserve actual atlas output. | W0 |
| CPU-003 | Measure per-update capture service, deadlines and scene age. | W0/W5 |
| GPU-01 | Trustworthy whole-frame/pass GPU evidence or explicit unavailability. | W0/W6 |
| GPU-02 | Same-frame scene-color input for adaptive overlays. | W6 |
| GPU-03 / ARC-004 | Bounded, cropped, independently versioned map draws. | W7 |
| CLI-01 | Cancel keyboard/actions on Android window-only replacement. | W1/W5 |
| CLI-02 | Completed-fresh-frame HUD FPS, distinct from submission cadence. | W1/W5 |
| ARC-001 | Shared truthful Creating/loading semantics. | W1/W5 |
| ARC-002 | Shared presentation/actions; constrain frame-loop responsibility. | W1/W5 |
| ARC-003 | Shared generation/final-map display-ice composition. | W5/W7 |
| ENV-001 | Remove the obsolete Snapshot-3 onboarding imperative. | W10 |
| ENV-002 | Reproducible exact private-package desktop setup. | W10 |
| ENV-003 | Accurate prerequisite-check exit status. | W10 |
| ENV-004 | Source-correct packaging examples and version provenance. | W10 |
| API-001 | Consistent scenario profile support documentation. | W9 |
| API-002 | Actual spawn-coordinate types, units and admission checks. | W9 |
| API-003 | Complete tick/operation/evidence budget accounting. | W9 |
| API-004 | All movement-component ranges documented. | W9 |
| API-005 | Complete policy callable contracts and diagnostics. | W9 |
| API-006 | World/generation callable contracts and parameter exports. | W9 |
| API-007 | Correct bare 0.1.5 source-header diagnostic. | W9 |
| API-008 | Enforced world-script execution/trust boundary. | W9 |

## Work order and acceptance owners

Add [W0 performance evidence](earlydev_0_1_1_s2_review_performance.md) alongside
W1 contract design. Establish baseline identities and counters before choosing
optimizations; remove confirmed unused work and address edited-column cost
with W2. Do not delay independent API/documentation and environment repairs
behind the external terrain-model decision.

[W9 scripting](earlydev_0_1_1_s2_review_scripting.md) owns execution boundaries
and complete documentation. Its [binding inventory](earlydev_0_1_1_s2_review_api.json)
retains 39 bindings, including ten private entries, plus parameter exports.
[Input, quality and W10 environment acceptance](earlydev_0_1_1_s2_review_actions.md)
adds the concrete platform/telemetry and setup checks.

The [work-package plan](earlydev_0_1_1_s2_verification.md) owns dependencies.
MC-AI-0483 owns W0/W9/W10 and the shared edit/protocol work; MC-AI-0516 owns
the input/loading/HUD follow-up; MC-AI-0484 and MC-AI-0515 retain adaptive/map
work. Use one metadata writer when creating bounded implementation tasks.
Keep an ordered per-item disposition with final source, relevant test/measurement
receipt and remaining limits. Builds alone cannot close runtime acceptance.

Accepted UI, gameplay scope, model-selection authority, inherited platform
thresholds and publication boundaries remain intact. S2 is still unimplemented
and unqualified; this planning update starts no reporting scheduler.
