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

Commit/push hooks own `ai_check.py --fast`, which always checks docs, backlog,
plan, index, whitespace, and receipts. Exact nonshared Python pairs use focused
tests. Native/build changes with docs metadata run source policy without
unrelated Python tests; unknown/shared tooling runs the full Python suite.
Source violations fail; ordinary dirty/version publisher failures are allowed
during development.

Full, candidate, and strict checks run the complete Python suite. Batch up to
8–10 tasks with disjoint ownership; run one full integration gate and required
runtime checks per batch, then push. Reuse only unchanged evidence; avoid
duplicate precommit checks and broad reviews.

Python tests have a 300-second budget by default and 420 seconds on Windows;
phase output and summary show elapsed time. The Windows runtime-staging CMake
fixture is limited to 60 seconds; timeout remains a failure and reports elapsed
time and captured partial output.

Full local, candidate, and strict CTest phases use a 240-second default test
timeout within a 2,400-second phase budget; explicit per-test timeouts apply.
The nested `MinecraftClone.ServerOnlyBuild` configures without the client,
builds all of `mc_tests`, and runs only
`GameServerTest.JoinRepliesArePrivateAndNewPlayersReachExistingClients`; it
fails if that test is unregistered. The outer suite runs the remaining tests.

After a snapshot's main gameplay features are usable, launch the normal macOS
candidate for the user's hands-on check before platform CI. Approval may unlock
the next snapshot's implementation in a separate task while the current one
finishes Windows/Android and artifact evidence; it does not authorize
publication or complete the release.

## Snapshots and versions

A snapshot records the preceding `ai-main` commit as `baseline_commit`. At
finalization collect **all basic-task commits since that boundary**, including
merged branches. Related branches may diverge, so include all reachable commits
outside the baseline and both parents of a pending promotion. Map imported
pre-policy commits explicitly; never omit changes or rewrite history.

Fill product changes first, code changes second; reconcile planned and actual children, then `ai_history.py finalize <id>`. It refuses missing/unfinished children and untraceable commits. Plans are retained beside actual results. Acceptance review must also establish that no promised product scope was silently dropped.

Snapshot closure runs the full suite, enabled code checks, and additional gates.
CI dependency, environment, and provenance checks fail closed; no arbitrary
check skip is allowed. The required snapshot/minor/major renderer-smoke gate
runs windowed `RendererSmokeTest`, fails on missing registration, device,
validation, test, or timeout errors, and records its receipt. It is not a
headless texture-golden gate; S4 owns offscreen comparisons. Then follow the
[publishing workflow](../VERSION_CONVENTION.md), including reviewed promotion
to `ai-main`.

Minor closure groups snapshot tasks; major closure groups minor tasks. Both repeat snapshot closure at their full scope and add Terra/high review, documentation sanity, environment verification, hindsight, and backlog maintenance. Record findings and fixes in the version task; create missing follow-up tasks, adjust priorities/dependencies, and repair justified skills/hooks/environment issues. The [release checklist](RELEASES.md) owns these gates.

Checkpoint only current state: completed acceptance, revision/diff, failing command, and exact next step. Resume from task and Git state. Creating planning tasks does not authorize publishing a release.
