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

Public task IDs are sequential; files and trailers retain `MC-AI-####`. The
[project skills](SKILLS.md) resolve them. Hierarchy: **basic → snapshot → minor → major**.
Parentage groups work; dependencies order it. Future stages remain plans.

## Basic task loop

1. Choose authorized work with `ai_tasks.py ready`; create missing tasks with `add`. Inspect status/diff, the task, and one relevant code guide.
2. Set owner and active status. Follow [model routing](MODELS.md); delegate only independent owned paths. The coordinator owns integration checks.
3. Implement the smallest accepted change with meaningful tests. Record unrelated findings as tasks. Update guides, hashes, and generated task documents.
   Asynchronous or mutable systems require bounded ownership, backpressure or cancellation, deterministic ordering, and headless contract coverage. Use deterministic clocks, inputs, and mocks for cross-system behavior.
4. Resolve only with actual changes and acceptance evidence. Stage only task changes and required traceability metadata.
5. Run one bounded **Luna/high review batch** for the exact staged task candidate. [Commit gates](COMMITS.md) bind the review to the exact staged tree; reuse that receipt only when inputs are unchanged and do not add a duplicate precommit wrapper. Fix genuine findings before commit; send only a changed delta to the same reviewer, and defer non-blocking cosmetic suggestions.
6. Commit once with one `Task-ID: MC-AI-####` trailer. Later changes require new tasks.

The commit/push hooks own `ai_check.py --fast`: docs, backlog, plan, index,
whitespace and receipt checks always run. Exact nonshared Python module/test
pairs use focused tests. Pure native/build changes with docs metadata run the
source-policy CLI and skip unrelated Python tests; unknown or shared tooling
uses the full Python suite. Source violations fail; ordinary dirty/version
publisher failures are allowed during development.

Full, candidate and strict checks always run the complete Python suite. Batch
up to 8–10 tasks with disjoint ownership. Run one full integration gate and
required runtime checks per batch, then push. Reuse evidence only for unchanged
inputs; avoid duplicate precommit checks and broad reviews.

After the main gameplay features of a snapshot are usable, launch the normal macOS candidate for the user's hands-on check before platform-CI work. User approval of that candidate may unlock the next snapshot's implementation in a separate task while the current snapshot finishes Windows/Android and artifact evidence; it does not authorize publication or mark the release complete.

## Snapshots and versions

A snapshot records the preceding `ai-main` commit as `baseline_commit`. At finalization, collect **all basic-task commits since that boundary**, including merged branches, rather than only changes after the latest basic commit. The original branches have related but divergent history; collection uses all reachable commits outside the baseline, including both parents of a pending promotion. Imported pre-policy commits require explicit task mappings. No silent omissions or history rewrites.

Fill product changes first, code changes second; reconcile planned and actual children, then `ai_history.py finalize <id>`. It refuses missing/unfinished children and untraceable commits. Plans are retained beside actual results. Acceptance review must also establish that no promised product scope was silently dropped.

Snapshot closure runs the full suite, all enabled code checks, and every enabled additional gate. CI dependency, environment, and provenance checks fail closed before expensive builds; no arbitrary check skip is allowed. The renderer-smoke gate is mandatory for snapshot, minor and major full checks: it runs the windowed noninteractive `RendererSmokeTest`, fails closed on missing registration, device/validation/test failures or timeouts, and records its receipt. It is not a fully headless texture-golden gate; S4 owns those deterministic offscreen comparisons. Then follow [the publishing workflow](../VERSION_CONVENTION.md), including a reviewed promotion commit into `ai-main`.

Minor closure groups snapshot tasks; major closure groups minor tasks. Both repeat snapshot closure at their full scope and add Terra/high review, documentation sanity, environment verification, hindsight, and backlog maintenance. Record findings and fixes in the version task; create missing follow-up tasks, adjust priorities/dependencies, and repair justified skills/hooks/environment issues. The [release checklist](RELEASES.md) owns these gates.

Checkpoint only current state: completed acceptance, revision/diff, failing command, and exact next step. Resume from task and Git state. Creating planning tasks does not authorize publishing a release.
