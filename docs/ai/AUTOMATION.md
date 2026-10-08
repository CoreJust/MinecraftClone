# Automation and extension points

Deterministic Python tools run locally without model calls or keys. Use Python 3.12+ (`python` on Windows when `python3` is unavailable).

| Tool | Purpose |
|---|---|
| `script/ai_plan.py` | Task/version lookup; [skills](SKILLS.md) |
| `script/ai_commit.py` | Review/commit validation; [protocol](COMMITS.md) |
| `script/ai_history.py` | Trace history and aggregate children |
| `script/ai_run.py <route>` | Local model runner |
| `script/ai_setup.py` | Prerequisite and hook setup |
| `script/ai_analysis_matrix.py` | Analysis receipts; see below |
| `script/ai_docs.py check` | Coverage and freshness |
| `script/ai_docs.py refresh` | Reviewed-doc refresh |
| `script/ai_tasks.py` | Task editing/rendering; see `--help` |
| `script/ai_check.py --fast` | Fast checks |
| `script/ai_check.py` | Build/test checks |
| `script/ai_check.py --strict` | Strict checks |

MC-AI-0249's exact-candidate matrix is declared in
[`script/ai_analysis_matrix.json`](../../script/ai_analysis_matrix.json). Run a
row, merge one passing receipt per row, then verify the merged candidate:

```sh
python3 script/ai_analysis_matrix.py run-row build/ai-checks/macos-asan.json \
  --row macos_asan --head <HEAD_SHA> --tree <INDEX_TREE_SHA>
python3 script/ai_analysis_matrix.py merge build/ai-checks/analysis-matrix.json \
  --row-receipt build/ai-checks/macos-asan.json --head <HEAD_SHA> --tree <INDEX_TREE_SHA>
python3 script/ai_analysis_matrix.py verify build/ai-checks/analysis-matrix.json \
  --head <HEAD_SHA> --tree <INDEX_TREE_SHA>
```

Receipts bind candidates, manifests, tools, commands and diagnostics; missing,
unavailable or stale rows fail closed. macOS runs ASan with integrated LSan,
UBSan, and TSan; Windows runs MSVC ASan/analysis; Android uses Release HWASan;
Linux uses tests-only LSan/MSan with libc++ and instrumented dependencies.
CTest rejects empty discovery, UBSan halts on diagnostics, and performance uses
ordinary Release. Local checks defer hosted receipts; CI binds them to immutable
trees before promotion and strict/tag checks.

The macOS ASan row keeps `detect_leaks=1` and runs clean, ordinary-leak, and real
AudioUnit callback controls before configuring/building the game. Leak reports
come from LSan integrated with ASan; Clang documents macOS leak detection via
`ASAN_OPTIONS=detect_leaks=1` and LSan integration in its
[AddressSanitizer guide](https://clang.llvm.org/docs/AddressSanitizer.html#memory-leak-detection).
Do not wrap the ASan-instrumented process with Apple `leaks`: on exact promotion
5fd704e, that external inspector could not inspect the ASan malloc zone
(job 113144469210; see [MC-AI-0384](tasks/MC-AI-0384.md)). Callback evidence
requires observed, successful execution; zero OSStatus for setup, start, stop,
uninitialize, and dispose; and one 4096-byte app-leak block with
both `createApplicationLeak` and `renderAndLeak`. Ordinary leaks must retain
their 4096-byte app frame. The three exact LSan sites are
`AMCP::Utility::Dispatch_Queue::install_mig_server`,
`AutoreleasePoolPage::autoreleaseNoPage`, and `__CFTSDGetTable`. Row receipts
retain suppression counts and unsuppressed reports. These exact stack-frame
rules can exclude allocations regardless of origin; they do not establish
harmlessness. Do not broaden them or hide other unsuppressed reports.

Before waiting for the aggregate, the exact-candidate gate waiter inspects the
latest push attempt and required jobs: terminal failure, cancellation, or skip
fails immediately; a missing job remains pending only while its workflow is
nonterminal and fails if still absent at completion. Success requires the
successful exact-candidate workflow and complete validated 13-row receipt.
Desktop diagnostics retain `build/ai-checks/python-tests.log` when present.

## Git hooks and CI

Install once per clone with `python3 script/ai_setup.py --install-hooks`. `core.hooksPath` is repository-local; installation does not change global settings. Remove this setting with `git config --local --unset core.hooksPath` to restore Git's default hook location. Existing hook installations require deliberate reconciliation, never silent replacement.

The hooks apply to `ai-dev`, `ai-main`, and `codex/ai-*` work. Pre-commit and pre-merge-commit require the exact Luna review receipt and run the one fast check for basic tasks or candidate release check for aggregates; do not wrap the same precommit predicate a second time. `ai_check.py` removes exactly the variables reported by `git rev-parse --local-env-vars` from the Python-test child environment; candidate and index checks retain the real hook environment. Commit-msg requires exactly one matching Task-ID trailer; post-commit refreshes ignored local task pages. Minor/major candidates also require Terra review. Pre-push checks the actual AI ref against the checked-out commit and requires a clean tree before running changed-scope fast checks for ordinary `ai-dev`/`codex/ai-*` refs; `ai-main` and `ai/*` tags retain the complete strict gate. The full build/CTest gate is reserved for the coordinator's explicit batch boundary or aggregate closure, not repeated by every basic commit. Pushing an AI checkout to the legacy `dev`/`main` refs is blocked. Hooks are guardrails, not remote branch protection, and can be bypassed by Git options.

Publication-ledger validation keeps snapshot planning and finalized change lists
immutable. One exact historical compatibility entry recognizes Snapshot 3 commit
`8df27fb8fa08d9e0cd625b8cad85209fdd09251d`, which predates that rule and
condensed four prose fields. The exception is commit-scoped; later ledgers use
only the standard publication fields.

[GitHub workflow](../../.github/workflows/ai-checks.yml) runs fast checks on pull requests and desktop plus analysis gates on AI-branch pushes. The Android HWASan runtime job is limited to `ai-main` pushes and requests a macOS ARM64 runner with a candidate-specific SHA/run-id label. Labels route jobs; they do not authorize access or prove that a runner is ephemeral, so runner registration and access controls remain external. See GitHub's [runner-selection](https://docs.github.com/en/actions/how-tos/write-workflows/choose-where-workflows-run/choose-the-runner-for-a-job) and [ephemeral-runner](https://docs.github.com/en/actions/reference/runners/self-hosted-runners) documentation. The workflow has read-only permissions and does not call a model, publish, or run arbitrary scheduled work. GPU/multiplayer acceptance belongs to supported runners and task evidence. Enabling remote rules or workflows requires separate repository administration; this setup does neither.

## Project skills and future hooks

The ten [project skills](SKILLS.md) share a deterministic planner. Prefer scripts for checks; add a skill only for repeated judgment work outside the [task loop](WORKFLOW.md). Candidates: Vulkan capture, worldgen comparison, and save-format migration.

A repository skill lives at `.agents/skills/<name>/SKILL.md`, with accurate frontmatter and under 500 words. Link owning guidance; test discovery, intended/unrelated triggers, and allowed/failing commands. Retire duplicates.

Provider hooks reuse tested scripts; never mutate code, hashes, task status, or publication. Avoid auto-approval/unrestricted shell; test hooks in a temporary repository.

## Runtime choices

[Project config](../../.codex/config.toml) selects Luna/high for new sessions, subject to overrides. It changes no sandbox, permissions, connections, or global settings. See the [routing policy](MODELS.md).

Codex [agent](https://developers.openai.com/codex/guides/agents-md), [skill](https://developers.openai.com/codex/skills), and [config](https://developers.openai.com/codex/config-basic) docs favor short entrypoints; tests and task policy are local decisions.
