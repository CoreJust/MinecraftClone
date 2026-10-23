# S2 — Carryover targets from S1

Planning only, 23 October 2026. The owner accepted the Windows S1 EXE
`cba2572c3cd47b9da82a2ad2ab1f958231ea20b57b9a6ddad17107013d89402b` for release
with known performance and LOD issues nonblocking. That release choice is not
performance or visual qualification. The S1 decision retains all 18 ordered
criteria; its original 36-hour allocation was missed. Neither is rewritten here.

The accepted four-screen prototype and design remain recorded at
`bf5047ac52f397179e8467998f6fe67604a6e5f5`. Native implementation, terrain-model
selection and platform qualification remain open. See the [S2 overview](earlydev_0_1_1_s2.md),
[verification plan](earlydev_0_1_1_s2_verification.md), [S1 decision](earlydev_0_1_1_s1.md)
and [S1 iteration plan](earlydev_0_1_1_s1_iterations.md). Carryover is tracked by
[MC-AI-0521](../tasks/MC-AI-0521.md). Original small/medium validation budgets remain
unresolved, not passed or waived.

## Recorded S1 evidence

| Area | Result and limit |
|---|---|
| Mac moving performance | Source-bound `mc_main` SHA-256 `0aacbd2ff732cc328e29b38509161a54c99b60706fcab0b98cf40495bc26fba7` met 8.333 ms / 120 FPS in 46.728364% of a 30-second window, below 99%. |
| Windows performance and loading | Historical RTX 5070 Ti, 1280×720, immediate-presentation run: about 79.77 fresh FPS; 0.042548% within 3.333 ms / 300 FPS (target 99%); prompt 5.6166 s / 4 s; 640-block readiness 5.511 s / 15 s. The raw record has no executable SHA. Its iteration lists “Windows staged df4e review049” only as a retained candidate boundary, not a binary identity. The run remains unqualified and unattributed; it does not apply to the separately accepted `cba2572c…` EXE. |
| Moving coverage | Mac strict capture had 6,480/6,561 required current columns at update 114, then `sources_current=false` at update 115. Neither full continuous coverage nor freshness is established. |
| Validation lanes | `fast-code` passed in 137.641 s; `ctest_core_small` timed out at 40.471 s; `ctest_acceptance_medium` failed at 39.564 s; other listed small/medium lanes were unrun. Full Python discovery timed out at 290.024/300 s after entering test 855, without summary. An earlier 822-test pass is not source-matched proof. |
| Android and visuals | Full Android visual/platform acceptance and Android performance are unestablished. S2 includes UI/lifecycle checks but no mobile FPS threshold. |

## S2 targets

| Target | Observable acceptance | Reproduction and evidence | Workstream and dependency |
|---|---|---|---|
| Fresh-frame performance and provenance | During moving x30 play, at least 99% of time meets Mac 120 FPS / 8.333 ms and Windows 300 FPS / 3.333 ms. Keep the 320-block Ready gate separate. Every result names the exact executable/package SHA-256 and source commit. | Repeat a 30-second normal-launch capture; bind raw timestamps, completed-frame IDs, coverage, hardware, display and settings. The historical Windows 0.042548% result lacks binary SHA and stays unattributed; reproduce against an identified immutable candidate. Stationary windows, builds and CPU microbenchmarks do not pass. | Renderer, platform-performance and measurement-provenance work; depends on final S1 source, identified production package, settings and normal launch. |
| Continuous R40 service | At x15, x30 and the recorded higher-seek workload, all 6,561 columns within 40 chunks / 640 blocks remain current in both authoritative and visible-submitted sets. Any stale source or 6,480/6,561 fails; this is separate from 320-block Ready. | Record both sets and source revisions through turns, reversals, borders and new terrain. | Streaming and renderer submission; depends on final S1 world identity and complete 320-block Ready. |
| Scene freshness | Displayed geometry matches current world/source identity; identify missing, evicted or delayed data from the first stale sample. A freshness boolean alone is insufficient. | Preserve scene and authority revisions at the first stale frame during movement and new-terrain arrival. | World, cache and renderer ownership; depends on the R40 capture path. |
| LOD and visual coherence | Matched mountain, river, lake, shoreline, snow/ice and border views stay coherent across LODs, materials, curvature and visibility; no holes, false walls or flicker. | Keep source-bound screenshots and seam evidence at matched locations/camera states, including [S2 curvature and visibility cases](earlydev_0_1_1_s2_verification.md). | Terrain rendering, materials and LOD; depends on selected terrain model and current world/scene identity. |
| Validation budgets and coverage | Complete source-matched small and medium lanes within <=3 and <=12 minutes, with full lane membership and no dropped coverage. | Bind source/configuration identity and input receipts; retain timeout/failure output and rerun failures first. | Validation tooling and lane owners; depends on verified lane inventory and available qualification hosts. |
| Loading, focus and platforms | Meet the 4-second Windows prompt target; verify cursor capture, relative look, focus loss/refocus and exit. Verify Android UI/maps, safe areas, input and lifecycle/resume; do not infer Android FPS. | Reproduce ordinary launch; retain prompt timing, focus/input steps and Android screenshots/runtime receipts. | Loading/input and Android presentation; depends on production startup and the [S2 focus/lifecycle contract](earlydev_0_1_1_s2_presentation.md). |

These targets extend the inherited [cursor](../tasks/MC-AI-0477.md),
[moving-performance](../tasks/MC-AI-0478.md), [coverage](../tasks/MC-AI-0479.md)
and [S2 work packages](../tasks/MC-AI-0483.md). Reconcile APIs and evidence with
final S1 source before implementation.

## Boundaries

World-model comparison remains active research; no candidate is an owner-approved
replacement. The six-file LOD bundle remains deferred at
`/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-freeze-01/deferred-lod/`.
Its exact-geometry CPU observation improved 305 ms to 94 ms (3.25×), but strict
normal-play capture failed. This is diagnostic only, not shipped behavior or S2
completion. Close targets only with a new source-bound result that meets its predicate;
retain raw failures and unqualified platform results.

## Source provenance

The four-screen S2 plan descends from `bf5047ac52f397179e8467998f6fe67604a6e5f5`.
The source target ledger and overview came from staged artifacts in parallel
`metadata-work` at HEAD `7b3f89e1f118fa3013c403f53658ebe92be4eb07`; source SHA-256
values were `e727125af792d5c9031acfc310d5f503db327ea496e8099707dda250b2351144`
and `f8b12c69ce70733fcf6f6a60f8f860783ba2612bfcfb345dec0ab448db4a07cc`.
This task adds the S1 Windows raw-measurement provenance gap as an S2 target; the
linked S2 overview remains unchanged from its source. This commit is planning only;
it does not implement S2 or approve a terrain model.
