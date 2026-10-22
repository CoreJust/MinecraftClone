# S2 — Work packages and acceptance

These are future implementation and acceptance checks for the
[S2 design](earlydev_0_1_1_s2.md). No checks below are claimed executed.
Only this planning task is being delivered; S2 stays unfinalized and S1 stays
the current snapshot.

## Ordered work packages

Each package includes its behavioral tests, production integration and owning
documentation. Create basic implementation tasks when execution is authorized;
do not mistake this outline for completed or scheduled work.

| Package | Deliverable and ownership | Prerequisites |
|---|---|---|
| W1 Shared contracts | Mode/policy, camera/voxel targeting, timing and typed HUD/climate producer contracts; shared world/policy and focused tests. | Reconcile final S1 APIs and design defaults. |
| W2 Canonical edit state | Sparse override storage independent of dense residency; versioned reads/materialization, collision and neighbor identities in `SparseWorld`/`Chunk`. | W1 world identities. |
| W3 Authoritative actions | Server aim/rights/timing checks, hold cancellation, placement transaction and mode requests; shared wire types and `GameServer`. | W1–W2. |
| W4 Live replication | Current-content replacement, per-chunk revisions, edited-column coverage, join/re-entry and resynchronization; server/client transport. | W2–W3. |
| W5 Interaction and UI | Desktop/Android input, text-first loading/game views, chunk/memory/climate HUD, target/cracks/particles and edited meshes; client presentation. | W1, W3–W4. |
| W6 Adaptive overlays | Sampleable scene/composite route, shared fixed/adaptive HUD and capture lifecycle; existing MC-AI-0484. | W5 frame/HUD data; may develop independently with fixtures. |
| W7 Minimap/world map | Shared map-view state, bounded atlas/local-tile sources, display/input, wrapping and edit invalidation; client presentation and renderer. | W2/W4 canonical edit state; W6 overlay route. |
| W8 Replacement model | Integrate the accepted research handoff with canonical sampling, edits and native generation/streaming. | Owner-selected qualified model; W2 contract. |

W1–W7 can establish editing and maps against the existing canonical world while the
separate model decision proceeds. That does not close W8 or authorize a
permanent substitute for it. Cross-package APIs and proposed file splits are
settled before parallel code writers start; metadata has one writer.

## Deterministic acceptance

| Area | Required evidence |
|---|---|
| Presets/policy | Exact three presets, Creative bedrock, no Survival flight, independent placement rules, empty whitelist/blacklist, hard veto and custom-rights HUD. |
| Geometry | All views, inclusive reach boundary, occlusion, negative/wrapped XY, world Z limits, corner/edge ties, starting inside a block and Unknown without materialization. Third-person cannot reach through walls or gain distance. |
| Breaking | Snow completes after 10 full ticks, Dirt/Grass 15, Ice 20, Stone 30; no initial-tick credit. Quick is first authorized tick. Duplicate input cannot accelerate progress. Same-cell aim jitter continues; release, expired hold, changed cell/rights/target or lost reach cancels. |
| Placement/modes | One press produces at most one edit; no Air/out-of-bounds/occupied-cell placement or collidable-body intersection. Conflicting actors serialize. Unsafe switches retain the complete old state; airborne flight removal uses normal gravity. |
| Storage | Air removals and return-to-base writes survive eviction/re-entry; edit more chunks than the configured residency cap without pinning those chunks. Replayed materialization cannot erase edits. |
| Stream | Duplicate/out-of-order edits, snapshot-before/after-delta, per-chunk gaps, first edit in a new vertical chunk, reconnect and changed world epoch converge. A skipped unrelated global edit ID is not a gap. |
| Full-volume coverage | A compact base profile cannot certify an edited column complete until its declared edit state is installed. Caves, overhangs, floating placements and top-block maps agree with authoritative queries. |
| Meshes | Face and wrapped neighbors rebuild; stale worker/capture/upload results cannot reintroduce solids or hide holes. Aim-only changes do not remesh terrain. |

Assert observable outcomes in the owning suites; never auto-bless goldens.

Map acceptance in the [map design](earlydev_0_1_1_s2_maps.md) additionally covers navigation, markers, zoom/detail, edited surfaces, wrapping, modality and bounded work.
The [UI design](earlydev_0_1_1_s2_ui.md) adds four-screen flow, telemetry units/sources, preserved FPS/uptime/speed/view/movement/XYZ/YPR/F1, conditional fields, dynamic/reduced-motion generation and current-renderer limits. Prototype screenshots are design evidence, not gameplay qualification.

## Visual, multiplayer and performance gates

Use the production shader route for a single glyph/crosshair over alternating
bright/dark pixels. Verify independent choices, fixed styling, alpha edges,
overlapping HUD without self-sampling, color encoding, resize, hot reload and
capture. Native play must show correct highlights/cracks, depth and curvature,
including grazing faces and first/rear/front views; a CPU ray test is insufficient.

Run ordinary local and remote two-player editing: one client already resident,
another joining after edits, simultaneous actions, travel away/back, held-input
loss and mode/permission changes. Include Android controls/lifecycle and the
required desktop/platform matrix; automated loopback is separate from physical
two-laptop and owner acceptance.

Carry final S1 loading, actual-volume, streaming and movement gates unchanged.
Current targets include at least 99% of moving time at 120 fresh FPS on Mac
and 300 on Windows at the agreed settings. Measure matched baseline/fixed/
adaptive workloads, CPU/GPU frame-time percentiles, edit-to-visible delay,
peak edit/queue storage and scene-target memory. An initial engineering target
is visible mesh correction within 100 ms of receiving an edit; this is a
proposal to validate, not a measured result or a replacement for the FPS gates.

## Remaining decision register

- External: accepted terrain-model identity, references and measured cost are
  still required from the research owner.
- Proposed: reach, placement palette, mode-access defaults, hold expiry and
  cosmetic limits are listed explicitly in the design; they remain editable.
- Implementation evidence: prove camera/curvature agreement and the selected
  sampleable attachment format against the final pinned dependencies.

Publication remains separately authorized. The implementation chat initializes
its required reporting lifecycle; this design request starts no scheduler.
