# Snapshot and version closure

Planning tasks are created on request before work. Current plans: [snapshot 0.1.0:3](tasks/MC-AI-0032.md), [minor 0.1.0](tasks/MC-AI-0033.md), [major 0.1](tasks/MC-AI-0034.md). None is released by creating its task.

| Task level | Children | Completion gates |
|---|---|---|
| Basic | — | Task metadata, minimal automatic checks, Luna/high commit review |
| Snapshot | Every basic task since previous snapshot boundary | Full build/test suite, all enabled code checks, enabled additional tests, product/code change lists, Luna/high review |
| Minor | All included snapshots | Snapshot gates, Terra/high review of changed parts, docs/environment sanity, hindsight and backlog maintenance |
| Major | All included minors | Minor gates across the entire major range, integration/compatibility acceptance, major hindsight |

## Finalize the plan

1. Keep `baseline_commit` immutable. Inspect `python3 script/ai_history.py collect <id>` and the canonical roadmap; the history graph must account for every commit and all promised product scope.
2. Complete basic children before their snapshot; complete snapshots before the minor and minors before the major. Add newly discovered tasks; do not remove unfinished planned work to make closure pass.
3. Fill `product_changes`, then `code_changes`. Use a truthful “No product behavior changed” for tooling-only work. Record `resolution_changes`, the actual `resolved_at` date, and concrete acceptance evidence.
4. Minor/major tasks also fill `docs_review`, `environment_review`, `retrospective`, and `backlog_review`. Include what was done, workflow/environment issues, actions taken, and follow-up task IDs. Check that README, contracts, task state and version history agree; fix justified environment/skill/hook issues and rerun affected checks.
5. Run `ai_history.py finalize <id>` to reconcile actual children/history and validate metadata. Finalized means ready for release gates, not published. Mark the task done only when its acceptance is established.
6. Stage the task finalization and obtain [the required review receipts](COMMITS.md). Before committing, run `ai_check.py --candidate --level snapshot` (or `minor`/`major`). Candidate checks allow only dirty Git state because the reviewed change is staged; snapshot metadata and every other enabled check must pass.

## Enabled checks

[ai_checks.json](../../script/ai_checks.json) is the extension registry. The full gate always runs build, the complete CTest suite, tooling/doc/task checks, whitespace, and `publish.py --checks-only` for all current source/style/version verifications. Enabled registry commands add tests without replacing existing checks. A failed or invalid enabled check blocks closure.

The screenshot entry is disabled with an explicit reason until screenshot tests are implemented. Enable its command when ready; snapshots, minors and majors then run it automatically. Minor/major environment diagnostics supplement the recorded environment review. A doctor command is not proof that every SDK/platform path works; record the supported-platform evidence and any unresolved limitation.

## Publish

Use [the version/branch procedure](../VERSION_CONVENTION.md). The finalization commit and the subsequent `ai-main` promotion commit reference the same aggregate task. Both need Luna review; minor/major candidates also need Terra's accumulated-change review. Validate the merge candidate before creating its commit, then strict checks after it is clean. Failed validation stops promotion; never create a passing receipt or release date merely to bypass a check.

Local commits/tags and remote publication are distinct. A request to create planning tasks does not authorize either release execution or remote mutation. A later release request authorizes its scoped local workflow; push/releases need the requested external authority. Record the actual published refs and artifacts in the aggregate's evidence.
