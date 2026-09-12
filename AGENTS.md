# AI development

Read [docs/ai/README.md](docs/ai/README.md), then only the guide and backlog item relevant to the task.

## Scope and branches

- Work on `ai-dev` or `codex/ai-<task>` branches based on it; `ai-main` receives tested snapshots while `dev` and `main` retain the original line.
- Check `git status` first; preserve staged/unstaged work and never bulk-stage unrelated changes.
- The request controls scope. Local reversible inspection, implementation, tests, and docs are authorized; publishing, remote changes, unrelated deletion, and history rewrites require authorization.

## Review and verification policy

Until the final pre-publication gate after the requested feedback hold, use one bounded, exact-task Luna/high review batch per immutable staged candidate. Review genuine correctness findings; for corrections, send only the delta to the same reviewer and renew the receipt. Defer non-blocking cosmetic suggestions. Do not add independent-range, dual-reviewer, or higher-model reviews without a request. At final pre-minor publication after the hold, add the accumulated-change review required by [the release policy](docs/ai/RELEASES.md), and no other boundary.

Keep one `Task-ID` trailer per commit, staged identity, automated evidence, Mac/Android acceptance, and separate user Windows runtime status; publication remains separately authorized. Honor review opt-out; deterministic/release gates still apply. After Snapshot 8, hold the minor for feedback; no automatic minor publication or Snapshot 9.

Maximize deterministic public-contract coverage: unit, subsystem integration, fixed-seed/replay, offscreen screenshot, and code checks. Run focused tests, formatters, and source-policy checks before final staging. Reuse receipts only when the checker confirms unchanged inputs; never skip checks or auto-bless goldens.

## Small context, fast execution

- Follow [model routing](docs/ai/MODELS.md) and only focused project skills; link shared contracts instead of duplicating them. Default to Luna/high and escalate per routing. Do not add global configuration or plugins.
- Use [project skills](docs/ai/SKILLS.md), start with `python3 script/ai_tasks.py ready`, and load the task, dependencies, and relevant [code guide](docs/code/README.md). Search before whole files.
- Use independent agents only when they save elapsed work. Give disjoint file ownership, short packets, and no copied conversation. Serialize backlog writes. Workers report the current command promptly when asked; the high-level coordinator consumes completed receipts, blockers, and product decisions rather than reconstructing periodic status or hashes.
- Every change needs a task; each task produces one commit with one `Task-ID` trailer. Review timing, staged identity, and release review follow the [review policy](#review-and-verification-policy), [task policy](docs/ai/WORKFLOW.md), and external publishing gate.
- Keep task notes current, not a transcript. Record issues in [BACKLOG.md](docs/ai/BACKLOG.md); do not implement unrelated findings.

## Delivery gates

- While iterating: focused tests, formatters, and source-policy checks; the commit hook owns the development gate, so callers do not run a duplicate precommit wrapper. Development `ai-dev` pushes use changed-scope fast validation.
- Before reporting completed work: `python3 script/ai_check.py` (build, tests, and `publish.py --checks-only`). Only dirty-tree and incorrect snapshot-file checks may be development exceptions; report them.
- Before an aggregate commit: `python3 script/ai_check.py --candidate --level snapshot` (or `minor`/`major`); after its commit and for `ai-main`/`ai/*` tags use full `--strict --level <level>` checks, plus runtime acceptance. A fast CI pass does not establish GPU or multiplayer correctness.
- When contracts change, update the owning guide; after reading it against source, run `python3 script/ai_docs.py refresh`, then `check`. A hash refresh is not semantic review.
- Complete backlog tasks only with acceptance evidence. Report changed scope, check results, and material limitations concisely.

## Code and tests

Follow [code conventions](docs/CODE_CONVENTIONS.md) and surrounding style: constants `SNAKE_CASE`, types and enum values `CamelCase`, functions `camelCase`, variables/files/namespaces `snake_case`. Do not rename files just to normalize style.
Comments explain a non-obvious contract, constraint, or algorithm, not visible syntax. Wrap either the whole expression on one line, the whole interior on one indented line, or one item per indented line; keep closing delimiters separate for multiline forms.
One test file corresponds to one suite and one source file or tightly related unit. Test observable contracts, common usage, and relevant edge cases; avoid implementation-mirroring assertions. `using namespace` is allowed in tests. Read an available `tests` skill before changing tests.
