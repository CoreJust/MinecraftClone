# S2 delivery execution

Goal: deliver the accepted S2 UI and authoritative 3D editing on released S1,
closing the 28 review dispositions and inherited gameplay issues.

Baseline is published S1 promotion `77655e5222a56d48ada99ff59e3dbd9c0f9cef6b`;
implementation starts from ledger `ce4d77b57c4e22f990e49131792ec760ae14137a`.
Accepted prototype remains `232eae681ddd9f38e6187a109168d70ce0b64ed5`.
The independent review plan `00fe25da` used task0522, which final S1 already
assigned to Windows analyzer findings. This reconciliation uses MC-AI-0524;
the released0522 record is preserved.

## Requirements and decisions

The [S2 requirements](earlydev_0_1_1_s2.md),
[verification contracts](earlydev_0_1_1_s2_verification.md),
[carryovers](earlydev_0_1_1_s2_delivery_targets.md) and
[review register](earlydev_0_1_1_s2_review.md) control implementation.
The coordinator maintains ordered acceptance/evidence in task evidence
`mc-s2-20261024/requirements.json`; passing tools alone cannot close product scope.

Owner steering on24October requires the new terrain model when its separate
work completes. Only after it lands, show model-driven snow on summits below
0C and add Sand for appropriate desert terrain. Owner clarification keeps generated terrain as a solid layered non-air column
with air above; no new cave/overhang generation. Sand includes suitable beaches
and deserts. Explicit player voxel edits still require canonical query, collision,
replication and mesh agreement; a base-only surface profile cannot certify edits. Further material ideas require model support and stay proportionate.

Engineering defaults remain5-block reach,100ms action cadence,300ms held lease,
one edit per actor/tick and existing policy hard vetoes, as proposed in the design.
New reporting uses the source tool cadence marker (<=4h minor, three per aggregate);
the heartbeat checks every3h and may close a shorter contiguous period.

## Dependency-ready work

- MC-AI-0525: durable sparse edits and untouched-column cost, W2.
- MC-AI-0526: pure modes, material rules, deterministic voxel targeting/timing, W1.
- MC-AI-0527: prerequisite doctor and desktop setup, W10.
- MC-AI-0528: complete public scripting contracts, W9 documentation.
- MC-AI-0529: skip unused progress snapshots, W0 confirmed work.
- MC-AI-0530: Android cancellation on surface replacement, W1/W5.
- Existing0484/0515/0516 retain adaptive overlays, maps and native UI.

After the shared world/action interfaces stabilize, implement serialized server
actions, revisioned snapshots/deltas and edited-column completeness before
interactive editing. Follow with native effects/UI and model integration;
parallel fixtures do not substitute for production integration.

Each bounded task gets failing contract evidence where behavior changes, focused
checks, one exact staged-candidate review and one Task-ID commit. The coordinator
owns shared manifests, generated docs, backlog and shared build/debug.

## Required closure

All S2 criteria and dispositions remain open until source-bound evidence passes.
Preserve S1 failed/unqualified metrics as history, not S2 exceptions. Run ordinary
play and visual diagnosis before expensive qualification; exact native/UI,
multiplayer, Mac/Windows performance and Android lifecycle evidence remain
separate from builds. Publication needs separate authority. The new-model
handoff is pending; independent work continues without treating that as completion.
