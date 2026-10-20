# S1 — Parallel major-iteration plan

**Planning only.** No development, scheduling or implementation starts here.
Plan only the **12h major objectives** now. At each major's actual start,
define its three 4h minor objectives using current evidence; no minor-task
schedule is fixed in advance. Finish early and move on immediately.

The initial allocation is **three major iterations, up to 36h**, replacing
the serial allocation. This is an objective budget, not a measured ETA.
Independent work overlaps; unresolved numerical,
dependency or performance problems trigger the owner's investigation/replanning
rules, not an unreported extension. Do not spend unused time to fill a period.

## Major objectives and concurrent work

| Major | Product work running concurrently | Supporting work running concurrently | Exit evidence |
|---|---|---|---|
| M1 — Native Terrain-v2 slice and usable integration seams | Global geology/coasts; hydrology/climate against frozen stage inputs; regional/local sampler and server seams; inherited cursor/frame/loading fixes; loading/2D adapters against defined production interfaces. | CoreLang exact dev-pin/backend integration; reference fixtures and numerical contracts; test/check classification and selector; reporting transition. Mac and Windows build and diagnose the first runnable slice concurrently. | Seed-42 native global stages match raw references; real sampler reaches an authoritative chunk and normal-game consumer; defect regressions close; platform smoke evidence identifies integration failures early. Shared validation inventory/selection and reporting meet the first major checkpoint. |
| M2 — Complete, reproducible S1 gameplay | Finish atlas and regional refinement; integrate canonical server/chunk/collision/material/LOD paths; integrate progressive map, safe spawn, key gate and 2D adapter. Replay/coverage and protocol lanes validate each merged increment immediately. | Both laptops compare the six seeds and backend identities concurrently; measure initial/full-radius loading and fresh frames as soon as available. Fix platform issues while unrelated features finish. Qualify selector/reporting budgets alongside feature work. | All S1 features work together; exact reference/material parity across six seeds, order/movement invariance, connected Ready coverage, real loading flow and multiplayer correctness. Obtain initial actual-GPU timing on both laptops, with target gaps and causes recorded. |
| M3 — Meet performance targets and close qualification | Mac and Windows tune/verify their targets concurrently; generation, streaming and renderer owners resolve independent bottlenecks. Run required Android correctness and protocol checks alongside work that does not contend for their devices. | Prepare packages, provenance, reports and acceptance evidence while tuning continues. Freeze the candidate when functional/performance gates pass; run the large matrix once, with independent platform lanes concurrent and paired-laptop tests scheduled together. | Original-T0 load deadlines, x30 sphere/FPS targets, deterministic/backends and real two-laptop host roles pass. All small/medium checks and required large qualification pass; owner acceptance/release gates and final report are recorded. Checked ai-dev checkpoint/readback; snapshot publication remains separately authorized. |

## Ownership and safe overlap

Assign disjoint files or isolated worktrees before writers start. One integrator
owns shared headers, build definitions, protocol identities and backlog writes.
Agree the small interface contracts first, then hand off increments as soon as
they pass focused checks; do not wait for a whole lane or major to finish.

| Lane | Owned responsibility | Earliest useful start / dependency |
|---|---|---|
| Global terrain | Geology/coasts; separate hydrology/climate owners where files permit | Immediately from frozen Terrain-v2 stage contracts; consumers can use retained raw stage fixtures while producers are ported. |
| World service | Regional/local generation, scheduler/cache, authoritative materialization/collision | Immediately on sampler/refinement contracts; final integration consumes the real atlas. |
| Client presentation | Material/LOD, loading map/key gate and shared 2D adapter | Immediately on production service/event contracts; acceptance requires the real generator. |
| Defects/platforms | Inherited regressions plus Mac and Windows native integration/performance | Defects immediately; both hosts validate each runnable merged slice. |
| Evidence/workflow | Fixtures, replay comparisons, classified checks/selector and reporting in both projects | Immediately and alongside product development; no separate prerequisite iteration. |
| Dependency | Exact CoreLang 0.1.5 ai-dev pin, JIT/AOT integration and fixes | Request handoff immediately; language-independent work continues while waiting. |

Fixtures unblock consumer implementation, never replace integrated acceptance.
The pin blocks CoreLang-specific qualification, not unrelated native work;
reporting setup blocks report delivery, not generator development. Defect
closure blocks affected acceptance, not independent atlas implementation.
If a supporting lane approaches a mandatory boundary, increase its capacity
without parking unrelated feature workers. Use useful available agent capacity,
not an arbitrary one-agent-per-major limit.

## Critical path and machine scheduling

The dependency chain is **numerical contracts → native atlas → authoritative
regional/chunk integration → actual loading/streaming → measured fixes → frozen
qualification**. Presentation, tests, dependency and workflow join only at the
boundaries they actually affect. Optimize the slowest prerequisite first.

Mac and Windows build/test independently on the same immutable merged source
and exact dependencies. Compare their evidence together and assign fixes by
module; do not finish Mac before starting Windows. Do not run heavy builds or
other benchmarks on a laptop during its performance measurement. Pair tests
need both laptops, so reserve them after independent measurements; the two host
roles run sequentially. Documentation and independent implementation continue
where resources permit. Full large validation remains ≤45 min on Mac, with
small ≤3 min and medium ≤12 min giving ≤60 min total; parallelism cannot hide
resource contention, skipped coverage or missed lane budgets.

Every major closes with all small/medium checks, at least one checked remote
ai-dev commit and a 12h report under the [shared workflow](project_iteration_workflow.md).
Define 4h tasks only at each major start. Close early when done; investigate
and replan missed goals instead of starting a buffer major.
