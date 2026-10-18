# AI development

Read [docs/ai/README.md](docs/ai/README.md), then only the guide and backlog item relevant to the task.

## Scope and branches

- Work on `ai-dev` or `codex/ai-<task>` branches based on it; `ai-main` receives tested snapshots while `dev` and `main` retain the original line.
- Check `git status` first; preserve staged/unstaged work and never bulk-stage unrelated changes.
- The request controls scope. Local reversible inspection, implementation, tests, and docs are authorized; publishing, remote changes, unrelated deletion, and history rewrites require authorization.

## Review and verification policy

Until the final pre-publication gate after the feedback hold, use one bounded exact-task Luna/high review per immutable staged candidate. Fix correctness findings through a delta to that reviewer and renew the receipt; defer cosmetics. Add no independent-range, dual-reviewer, or higher-model reviews unless requested. At final pre-minor publication, follow [release policy](docs/ai/RELEASES.md)'s accumulated-change review.

Each commit has one `Task-ID` trailer. Keep Mac/Android acceptance separate from Windows runtime status; publication requires separate authority. Honor review opt-out but retain deterministic gates. After S7, hold minor feedback; no automatic minor publication or next snapshot.

Cover public contracts with unit, integration, replay, offscreen screenshot, and code checks. Run focused tests, formatters, and policy checks before staging; reuse only checker-validated receipts. Never skip checks or auto-bless goldens.

## Small context, fast execution

- Follow [model routing](docs/ai/MODELS.md) and only focused project skills; link shared contracts instead of duplicating them. Default to Luna/high and escalate per routing. Do not add global configuration or plugins.
- Use [project skills](docs/ai/SKILLS.md), start with `python3 script/ai_tasks.py ready`, and load the task, dependencies, and relevant [code guide](docs/code/README.md). Search before whole files.
- Use independent agents only when they save elapsed work. Give disjoint file ownership, short packets, and no copied conversation. Serialize backlog writes. Workers report the current command promptly when asked; the high-level coordinator consumes completed receipts, blockers, and product decisions rather than reconstructing periodic status or hashes.
- Every change needs a task; each task produces one commit with one `Task-ID` trailer. Review timing, staged identity, and release review follow the [review policy](#review-and-verification-policy), [task policy](docs/ai/WORKFLOW.md), and external publishing gate.
- Keep task notes current, not a transcript. Record issues in [BACKLOG.md](docs/ai/BACKLOG.md); do not implement unrelated findings.

## Delivery gates

- At release start, initialize the managed report folder and ordered feature manifest. Follow [release reporting](docs/ai/RELEASE_REPORTS.md) for three-hour PDFs, fourth-period twelve-hour consolidation, and evidence-checked cleanup only after verified full delivery.

- While iterating: run the affected module's focused build/tests and `python3 script/ai_check.py --fast`; the fast gate does not run the batch-wide build or CTest suite. The commit hook owns this development gate, so callers do not run a duplicate precommit wrapper.
- Batch boundaries require one full integration gate plus batch checks before push; do not rerun per task.
- Before reporting a snapshot or release complete, use the aggregate candidate/strict gates below and report missing platform or runtime evidence rather than substituting unit tests.
- Before an aggregate commit: `python3 script/ai_check.py --candidate --level snapshot` (or `minor`/`major`); after its commit and for `ai-main`/`ai/*` tags use full `--strict --level <level>` checks, plus runtime acceptance. A fast CI pass does not establish GPU or multiplayer correctness.
- Run macOS GUI acceptance with host authorization. Retry sandbox-only launch failures; never reset Launch Services or reboot.
- When contracts change, update the owning guide; after reading it against source, run `python3 script/ai_docs.py refresh`, then `check`. A hash refresh is not semantic review.
- Complete backlog tasks only with acceptance evidence. Report changed scope, check results, and material limitations concisely.

## Code and tests

Follow [code conventions](docs/CODE_CONVENTIONS.md): constants `SNAKE_CASE`, types/enum values `CamelCase`, functions `camelCase`, variables/files/namespaces `snake_case`; avoid style-only renames. Comments explain non-obvious contracts, constraints, or algorithms; follow documented multiline style. Keep one test suite per file for one source or tightly related unit. Test public contracts, common usage, and relevant edge cases, not implementation details; `using namespace` is test-only. Read the `tests` skill before editing tests.
