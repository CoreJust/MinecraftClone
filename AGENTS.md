# AI development

Read [README](docs/ai/README.md), [next-minor handoff](docs/ai/INITIATION-CLOSURE-2026-10-19.md), and the task's guide/backlog.

## Scope and branches

- Use `ai-dev` or `codex/ai-<task>` from it; publish tested snapshots to `ai-main`, preserving `dev` and `main`.
- Check `git status` first; preserve staged/unstaged work and never bulk-stage unrelated changes.
- Before resume edits, verify task root, branch, and HEAD; the project root may not contain the active candidate.
- The request controls scope. Local reversible inspection, implementation, tests, and docs are authorized; publishing, remote changes, unrelated deletion, and history rewrites require authorization.

## Review and verification policy

Until the final pre-publication gate after the feedback hold, use one exact-task Luna/high review batch per immutable staged candidate. Fix correctness findings through a delta to the same reviewer and renew the receipt; defer cosmetic suggestions. Do not add independent-range, dual-reviewer, or higher-model reviews without a request. At final pre-minor publication, add the accumulated-change review in [release policy](docs/ai/RELEASES.md).

Each commit has one `Task-ID` trailer and exact staged evidence. Keep Mac/Android acceptance separate from user Windows runtime status. Publication needs separate authority. Honor review opt-out but retain deterministic gates. After Snapshot 7, hold the minor for feedback; do not publish it or start another snapshot automatically.

Cover public contracts with unit, integration, fixed-seed/replay, offscreen screenshot, and code checks. Run focused checks before staging. Reuse only checker-validated receipts; never skip checks or auto-bless goldens.

## Small context, fast execution

- Follow [model routing](docs/ai/MODELS.md) and only focused project skills; link shared contracts instead of duplicating them. Default to Luna/high and escalate per routing. Do not add global configuration or plugins.
- Use [project skills](docs/ai/SKILLS.md), start with `python3 script/ai_tasks.py ready`, and load the task, dependencies, and relevant [code guide](docs/code/README.md). Search before whole files.
- Delegate independent work with disjoint ownership. Packets identify root, branch, HEAD and owned paths. Mutating calls verify `workdir`, `pwd -P`, Git root, branch, HEAD and relevant status; file edits use absolute owned paths. Stop on mismatch. The coordinator checks diffs/status, serializes backlog writes and consumes receipts.
- Every change needs a task; each task produces one commit with one `Task-ID` trailer. Review timing, staged identity, and release review follow the [review policy](#review-and-verification-policy), [task policy](docs/ai/WORKFLOW.md), and external publishing gate.
- Keep task notes current, not a transcript. Record issues in [BACKLOG.md](docs/ai/BACKLOG.md); do not implement unrelated findings.

## Delivery gates

- Automatically initialize implementation's folder/heartbeat for [three-hour PDFs, twelve-hour consolidation, fixed features, time diagrams and evidence-checked cleanup](docs/ai/RELEASE_REPORTS.md).

- Bound stages below 30 minutes before launch; persist expiry/owned-process identities in receipts for resume. Preflight target, prerequisites and recovery. Print test names; for silence over 30 seconds announce phase/duration/next update, report every 60 seconds, and terminate owned trees at deadline.
- Split work exceeding a deadline into bounded stages. Retain partial logs and a failure receipt, mark later stages unrun, and resume from the command or checkpoint; partial output never passes. `ai_check.py` caps stages at 10 minutes and invocations at 25.
- While iterating, run focused checks and `python3 script/ai_check.py --fast`. The commit hook owns this gate; do not rerun its wrapper.
- At each batch boundary, run one full integration gate and batch checks before push, not once per task.
- Before aggregate commit, run `python3 script/ai_check.py --candidate --level snapshot` (or `minor`/`major`). After commit and for `ai-main`/`ai/*` tags, run `--strict --level <level>` plus runtime acceptance. Fast CI does not prove GPU or multiplayer correctness.
- macOS Cocoa/GLFW: outer `exec_command(sandbox_permissions=require_escalated)`, including worker packets. No sandbox-first probes, Launch Services resets or reboots.
- Refresh changed restored inputs' mtimes; preserve unchanged ones and check build graphs. Use CMake's configured tool even for diagnostics: mismatched Ninja can rewrite logs.
- When contracts change, update the owning guide; after reading it against source, run `python3 script/ai_docs.py refresh`, then `check`. A hash refresh is not semantic review.
- Complete backlog tasks only with acceptance evidence. Report changed scope, check results, and material limitations concisely.

## Code and tests

Follow [code conventions](docs/CODE_CONVENTIONS.md): constants `SNAKE_CASE`, types/enum values `CamelCase`, functions `camelCase`, and variables/files/namespaces `snake_case`. Avoid style-only renames. Comments explain non-obvious contracts/constraints; match documented multiline style.
One test suite per file, covering one source or tightly related unit. Assert observable contracts, not implementation details; `using namespace` is test-only. Read `tests` before adding tests.
