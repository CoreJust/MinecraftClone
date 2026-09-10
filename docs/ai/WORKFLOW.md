# Task-driven development policy

Every change belongs to a task created before work. [backlog.json](backlog.json) is canonical; `ai_tasks.py render` creates the backlog and a separate [task document](tasks/MC-AI-0031.md) for each record. Use `show <id>` for small context. GitHub issues may supplement these records without becoming a second independent task state.

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
| Commit relation | Exactly one `Task-ID` trailer per commit; derived hashes and GitHub links |

Public task IDs are sequential numbers; file/trailer IDs retain the `MC-AI-####` namespace. The [project skills](SKILLS.md) resolve them deterministically. Basic tasks may be unassigned in the backlog; assign active implementation to the current snapshot. Hierarchy: **basic → snapshot → minor → major**. Parentage groups work; dependencies order it. Aggregates are created on request before implementation, initially with plans and unresolved resolution fields. Future stages remain plans, not completion claims.

## Basic task loop

1. Choose authorized work with `ai_tasks.py ready`; create missing tasks with `add`. Inspect status/diff, read the task and one relevant code guide. Record motivation/context before editing.
2. Set owner and active status. Follow [model routing](MODELS.md); delegate only independent owned paths. The coordinator owns full integration checks; workers run focused tests and do not start their own review pipelines.
3. Implement the smallest accepted change, including meaningful tests. Record unrelated findings as tasks. Update affected guides, explicitly refresh documentation hashes, and generate task documents.
4. Resolve the task only with actual resolution date, changes, and acceptance evidence. Stage only its changes; metadata updates required for its parent/traceability belong to the same task.
5. Run minimal checks and a **Luna/high review before every commit**, including documentation and merge commits. [Commit gates](COMMITS.md) bind the review to the exact staged tree. Fix findings and renew the receipt if staging changes.
6. Commit with exactly one trailer: `Task-ID: MC-AI-####`. One task may have several commits. The post-commit hook refreshes local task pages with exact commit backlinks.

A routine local report still runs `ai_check.py`; the minimal commit gate is `--fast`. Do not add repeated broad reviews between successful focused checks.

## Snapshots and versions

A snapshot records the preceding `ai-main` commit as `baseline_commit`. At finalization, collect **all basic-task commits since that boundary**, including merged branches, rather than only changes after the latest basic commit. The original branches have related but divergent history; collection uses all reachable commits outside the baseline, including both parents of a pending promotion. Imported pre-policy commits require explicit task mappings. No silent omissions or history rewrites.

Fill product changes first, code changes second; reconcile planned and actual children, then `ai_history.py finalize <id>`. It refuses missing/unfinished children and untraceable commits. Plans are retained beside actual results. Acceptance review must also establish that no promised product scope was silently dropped.

Snapshot closure runs the full suite, all enabled code checks, and every enabled additional gate. The renderer-smoke gate is mandatory for snapshot, minor and major full checks: it runs the windowed noninteractive `RendererSmokeTest`, fails closed on missing registration, device/validation/test failures or timeouts, and records its receipt. It is not a fully headless texture-golden gate; S4 owns those deterministic offscreen comparisons. Then follow [the publishing workflow](../VERSION_CONVENTION.md), including a reviewed promotion commit into `ai-main`.

Minor closure groups snapshot tasks; major closure groups minor tasks. Both repeat snapshot closure at their full scope and add Terra/high review, documentation sanity, environment verification, hindsight, and backlog maintenance. Record findings and fixes in the version task; create missing follow-up tasks, adjust priorities/dependencies, and repair justified skills/hooks/environment issues. The [release checklist](RELEASES.md) owns these gates.

Checkpoint only current state: completed acceptance, revision/diff, failing command, and exact next step. Resume from task and Git state. Creating planning tasks does not authorize publishing a release.
