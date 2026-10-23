# S2 — Input, responsibility and environment follow-up

Future acceptance for the [review register](earlydev_0_1_1_s2_review.md).
Reconcile the audited source with final S1; none of these checks has passed as
a result of this planning change. Existing
[presentation](earlydev_0_1_1_s2_presentation.md),
[generation](earlydev_0_1_1_s2_generation_ui.md) and
[map](earlydev_0_1_1_s2_map_pipeline.md) contracts retain their authority.

## W1/W5 input and telemetry

**CLI-01:** cancel held keyboard/gamepad latches and queued edge actions at
Android surface teardown through the same semantic cancellation boundary as
focus loss/pause. Retain the connection/session. Verify press while Playing,
lost release, `TERM_WINDOW` then `INIT_WINDOW` without a focus/pause callback:
the next submitted input is neutral until a fresh press. Cover ascend,
descend and capability-cycle actions, normal releases, pause/focus loss and
connection reset. Device frequency was not measured by the audit; deterministic
command-sequence coverage and ordinary Android lifecycle proof remain separate.
Apply this boundary to S2 held breaking and map/edit inputs.

**CLI-02:** derive ordinary HUD FPS from unique completed fresh-frame identities
and their observation times, including swapchain generation. Keep submission
cadence separately named if exposed; show unavailability honestly. Submit
multiple frames while withholding completion: HUD completion counts must not
advance. Then verify once-only counting, duplicate IDs, slot reuse and
recreation. Preserve the existing fence-confirmed benchmark verifier; the audit
found a HUD contract mismatch, not a defective benchmark or measured FPS error.

**ARC-001:** one shared semantic snapshot must distinguish Creating/Connecting,
real generation, preparation, Ready/start acknowledgement, failure, cancellation
and retry. Test the state matrix and native desktop/Android views. Creating
must not emit synthetic `Tectonics 0/0`, advance progress or alter readiness.

## Responsibility and shared map data

**ARC-002:** add S2 through immutable UI-frame/semantic-action ports. The same
client inputs produce the same semantic frame on desktop and Android;
rendering consumes it without querying `GameClient`/world state or mutating
authority/readiness. Test transitions without Vulkan. Keep platform event/layout
adapters separate from shared screen/input policy. Large files are evidence of
ownership concentration, not an arbitrary line-count limit or a mandate for a
whole-project rewrite. Preserve renderer-free targets and focused dependency
boundaries when extending the modules.

**ARC-003, W5/W7:** generation and final maps consume one identity/epoch-matched
display-ice mask and compositor. Verify threshold boundaries and exact final
membership/pixels through Climate/Arrival and world-map views. The audited
S1 ocean tint used −2 °C while the accepted display proxy uses water at/below
−10 °C and Snow-zone land. Reconcile those paths using the existing accepted
generation design; do not turn the proxy into terrain physics or change R6.

## W10 environment and documentation

| Item | Planned correction | Observable acceptance |
|---|---|---|
| ENV-001 | Replace the historical Snapshot-3 imperative with current task-selection guidance; retain genuine history as history. | Entry-point instructions agree with `current.json` and the selected task; task/doc integrity checks pass. |
| ENV-002 | Document exact CoreCpp/CoreProject2026 acquisition/build/install-prefix setup for desktop, including required access and package lookup. | A clean desktop without ambient private packages follows the guide, resolves locked revisions and rejects mismatched package provenance. |
| ENV-003 | Make the documented prerequisite check fail for missing/invalid vcpkg toolchain roots, or explicitly separate informational output from a failing preflight. | With CMake/Ninja present, missing root and absent toolchain file fail; a valid pinned root passes. Preserve read-only doctor behavior. |
| ENV-004 | Use source-derived versions or unmistakable placeholders in packaging commands and matching filenames. | Examples cannot label current inputs as 0.1.0:3; retain the source/version mismatch rejection in the release workflow. |

W10 repairs guides and focused tooling behavior without changing dependencies,
credentials, global hooks, CI policy, release authority or business metadata.
Version/prefix examples depend on final S1 identity; setup work can proceed
alongside W1/W9. Validate affected guides against actual commands, then run
documentation/task checks and focused tool tests when implementation changes
behavior. Existing small/medium lane budgets and failed/unrun evidence remain
in the S1 carryover plan; a documentation pass does not close those gates.
