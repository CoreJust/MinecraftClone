# Automation and extension points

All deterministic tools are local Python scripts, require no model calls or keys, and run from any working directory. Use Python 3.12+ (`python` on Windows if `python3` is unavailable).

| Tool | Purpose |
|---|---|
| `script/ai_plan.py` | Numeric task and current-version lookup, unassigned backlog, planning and implementation preparation; [skills](SKILLS.md) |
| `script/ai_commit.py` | Candidate identity, local Luna/Terra review receipts, and commit validation; see [protocol](COMMITS.md) |
| `script/ai_history.py` | Trace commits, collect/finalize aggregate children, refresh local task backlinks |
| `script/ai_run.py <route>` | Explicit local model/high preset; `--dry-run` prints the command without invoking a model |
| `script/ai_setup.py` | Read-only prerequisite report; `--install-hooks` installs this repository's Git hooks without replacing existing active hooks |
| `script/ai_docs.py check` | Coverage, links, word limits, index/source-state freshness |
| `script/ai_docs.py refresh` | Explicitly acknowledge guide review and regenerate mechanical artifacts |
| `script/ai_tasks.py` | Validate, render, add/update, and list ready backlog items; see `--help` |
| `script/ai_check.py --fast` | Documentation/planner checks, Python tooling tests, whitespace |
| `script/ai_check.py` | Fast checks plus debug build, CTest, and publisher checks-only |
| `script/ai_check.py --strict` | Full checks with no development publisher exceptions |

## Git hooks and CI

Install once per clone with `python3 script/ai_setup.py --install-hooks`. `core.hooksPath` is repository-local; installation does not change global settings. Remove this setting with `git config --local --unset core.hooksPath` to restore Git's default hook location. Existing hook installations require deliberate reconciliation, never silent replacement.

The hooks apply to `ai-dev`, `ai-main`, and `codex/ai-*` work. Pre-commit and pre-merge-commit require the exact Luna review receipt and run fast checks for basic tasks or candidate release checks for aggregates. `ai_check.py` removes exactly the variables reported by `git rev-parse --local-env-vars` from the Python-test child environment; candidate and index checks retain the real hook environment. Commit-msg requires exactly one matching Task-ID trailer; post-commit refreshes ignored local task pages. Minor/major candidates also require Terra review. Pre-push checks the actual AI ref against the checked-out commit and requires a clean tree before running the full gate; `ai-main` requires strict checks. Pushing an AI checkout to the legacy `dev`/`main` refs is blocked. Hooks are guardrails, not remote branch protection, and can be bypassed by Git options.

[GitHub workflow](../../.github/workflows/ai-checks.yml) runs fast checks on AI branches and pull requests. It has read-only permissions and does not call a model, publish, or run arbitrary scheduled work. Linux CI validates tooling only: the application supports macOS and Windows. GPU/multiplayer acceptance belongs to a supported runner and the task evidence. Enabling remote rules or workflows requires publishing and separate repository administration; this setup performs neither.

## Project skills and future hooks

The ten [project skills](SKILLS.md) use a shared deterministic planner for task identity and selection. Prefer a script for a deterministic predicate. Add a skill only when a repeated judgment-heavy workflow is not covered by the [task loop](WORKFLOW.md). Candidate skills: Vulkan validation/capture triage; deterministic worldgen comparison; save-format migration. Implement each when its subsystem exists and a real repeated task justifies it.

A repository skill lives at `.agents/skills/<name>/SKILL.md`, with `name` and a precise `description` in YAML frontmatter. Keep the body under 500 words; link the owning guide and commands instead of copying them. Verify discovery, one intended trigger, one unrelated non-trigger, and a failed/allowed command case. Retire duplicated or ineffective guidance.

Provider hooks must call the same tested scripts and must never mutate code, refresh hashes, mark tasks done, or publish on stop. Add no auto-approval or unrestricted shell settings. Maintain one implementation per check. Test any new hook in a temporary repository before local installation.

## Runtime choices

[Project config](../../.codex/config.toml) selects Terra/high for new Codex sessions in a trusted project. App/task/CLI overrides may take precedence. It changes no sandbox, permissions, MCP connections, or account configuration. The [routing policy](MODELS.md) specifies which model each delegated task uses.

Current vendor conventions were checked on 2026-09-09: [AGENTS.md](https://developers.openai.com/codex/guides/agents-md), [skills](https://developers.openai.com/codex/skills), [project config](https://developers.openai.com/codex/config-basic). They support short entrypoints and loading detailed guidance on demand; this repository's tests, task schema, and update policy are project decisions.
