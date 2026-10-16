# Task-driven development policy

Every change belongs to a task. [backlog.json](backlog.json) is canonical;
`ai_tasks.py render` creates the backlog and task documents. GitHub issues may
supplement these records without becoming separate task state.

## Task contract

| Metadata | Rule |
|---|---|
| Identity | Stable ID, title, kind, level, parent, status, owner, model route |
| Motivation | Why the change matters |
| Pre-existing context | Relevant behavior, evidence, constraints and inherited work |
| Importance | `priority`: P0 blocking, P1 next delivery, P2 planned, P3 optional |
| Complexity | low, medium, high; separate from importance |
| Dates | `created_at`; `resolved_at` on resolution, both ISO dates |
| Plan and acceptance | Intended scope, dependencies, observable completion criteria |
| Resolution | Actual changes and verification evidence; do not backfill invented results |
| Change lists | Product changes first, code changes second |
| Commit relation | Exactly one task commit with one `Task-ID` trailer; derived hashes and GitHub links |

Public IDs are sequential; files and trailers use `MC-AI-####`. See the
[project skills](SKILLS.md). Hierarchy: **basic → snapshot → minor → major**;
parentage groups tasks and dependencies order them. Future stages remain plans.
An aggregate dependency is satisfied only when finalized, so implementation can
continue while a completed snapshot awaits gates or feedback. Active aggregates
and unfinished basic tasks remain unresolved dependencies.

## Basic task loop

1. Choose authorized work with `ai_tasks.py ready`; add missing tasks. Inspect status/diff, the task, and one relevant code guide.
2. Set owner/status and follow [model routing](MODELS.md). Delegate only independent paths; the coordinator owns integration checks.
3. Implement the smallest accepted change with meaningful tests. Record unrelated findings as tasks; update guides, hashes, and generated task docs. Async or mutable systems need bounded ownership, backpressure or cancellation, deterministic ordering, and headless contract coverage; use deterministic clocks, inputs, and mocks.
4. Resolve only with actual changes and acceptance evidence. Stage only task changes and required traceability metadata.
5. Run one bounded **Luna review batch** at the selected effort (including **max** when selected) for the exact staged candidate. [Commit gates](COMMITS.md) bind the receipt to that tree; reuse only unchanged inputs and avoid duplicate precommit wrappers. Fix genuine findings through a delta to the same reviewer; defer non-blocking cosmetics.
6. Commit once with one `Task-ID: MC-AI-####` trailer. Later changes require new tasks.

On resume, `ai_plan.py implement-snapshot` marks `done` tasks lacking a
reachable or imported `Task-ID` commit as `done-uncommitted`; recover and commit
them before continuing.

Basic commit hooks check the reviewed index export, preserve working files and
retain failed logs. Push hooks run `ai_check.py --fast`. Checks cover docs, tasks,
plans, index and whitespace. Exact Python pairs use focused tests; shared
tooling uses the full suite. Native/build changes with metadata use source
policy. Source violations fail; development permits only dirty/version
publisher exceptions.

Full, candidate, and strict checks run the complete Python suite. Batch up to
8–10 tasks with disjoint ownership; run one full integration gate and required
runtime checks per batch, then push. Reuse only unchanged evidence; avoid
duplicate precommit checks and broad reviews.

Cap automated stages at 600 seconds, local analyzer matrices and `ai_check.py`
invocations at 1,500, and each GitHub artifact job at 30 minutes. Python tests
get 300 seconds (420 on Windows), renderer smoke 600, CTest 600 per phase
(240 per test), Android emulator stages and analyzer rows 600. Timeouts retain
partial logs/failing receipts and mark remaining checks unrun; split work to
resume rather than extending a gate.

The Windows runtime-staging CMake fixture is limited to 60 seconds; timeout
remains a failure and reports elapsed time and captured partial output.

The nested `MinecraftClone.ServerOnlyBuild` configures without the client,
builds all of `mc_tests`, and runs only
`GameServerTest.JoinRepliesArePrivateAndNewPlayersReachExistingClients`; it
fails if that test is unregistered. The outer suite runs the remaining tests.

After a snapshot's main gameplay features are usable, launch the normal macOS
candidate for the user's hands-on check before platform CI. Approval may unlock
the next snapshot's implementation in a separate task while the current one
finishes Windows/Android and artifact evidence; it does not authorize
publication or complete the release.

Follow the [qualification preflight and recovery rules](QUALIFICATION.md) before expensive gates or resuming failed phases.

## Snapshots and versions

A snapshot records the preceding `ai-main` commit as `baseline_commit`. At
finalization collect **all basic-task commits since that boundary**, including
merged branches. Related branches may diverge, so include all reachable commits
outside the baseline and both parents of a pending promotion. Map imported
pre-policy commits explicitly; never omit changes or rewrite history.

Fill product changes first, code changes second; reconcile planned and actual children, then `ai_history.py finalize <id>`. It refuses missing/unfinished children and untraceable commits. Plans are retained beside actual results. Acceptance review must also establish that no promised product scope was silently dropped.

Snapshot closure runs the full suite, enabled code checks, and additional gates.
Local dependency, environment, qualification and provenance checks fail closed;
GitHub only builds artifacts. Required proof reuse must match the check's relevant
inputs, not merely a previous successful run. The required snapshot/minor/major renderer-smoke gate
runs windowed `RendererSmokeTest`, fails on missing registration, device,
validation, test, or timeout errors, and records its receipt. It is not a
headless texture-golden gate; S4 owns offscreen comparisons. Then follow the
[publishing workflow](../VERSION_CONVENTION.md), including reviewed promotion
to `ai-main`.

Minor closure groups snapshot tasks; major closure groups minor tasks. Both repeat snapshot closure at their full scope and add Terra/high review, documentation sanity, environment verification, hindsight, and backlog maintenance. Record findings and fixes in the version task; create missing follow-up tasks, adjust priorities/dependencies, and repair justified skills/hooks/environment issues. The [release checklist](RELEASES.md) owns these gates.

Checkpoint only current state: completed acceptance, revision/diff, failing command, and exact next step. Resume from task and Git state. Creating planning tasks does not authorize publishing a release.
