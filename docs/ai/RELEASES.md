# Snapshot and version closure

Planning tasks are created on request before work. Current plans: [snapshot 0.1.0:3](tasks/MC-AI-0032.md), [minor 0.1.0](tasks/MC-AI-0033.md), [major 0.1](tasks/MC-AI-0034.md). None is released by creating its task.

| Task level | Children | Completion gates |
|---|---|---|
| Basic | — | Task metadata, minimal automatic checks, Luna/high commit review |
| Snapshot | Every basic task since previous snapshot boundary | Full build/test suite, all enabled code checks, enabled additional tests, product/code change lists, Luna/high review |
| Minor | All included snapshots | Snapshot gates, final pre-publication Terra/high review of accumulated changes after the user-feedback hold, docs/environment sanity, hindsight and backlog maintenance |
| Major | All included minors | Minor gates across the entire major range, integration/compatibility acceptance, major hindsight |

## Finalize the plan

1. Keep `baseline_commit` immutable. Inspect `python3 script/ai_history.py collect <id>` and the canonical roadmap; the history graph must account for every commit and all promised product scope.
2. Complete basic children before their snapshot; complete snapshots before the minor and minors before the major. Add newly discovered tasks; do not remove unfinished planned work to make closure pass.
3. Fill `product_changes`, then `code_changes`. Use a truthful “No product behavior changed” for tooling-only work. An active snapshot records local gate evidence and `resolution_changes` without a release date; only the post-publication `done` ledger records the actual `resolved_at` date and external artifact evidence.
4. Minor/major tasks also fill `docs_review`, `environment_review`, `retrospective`, and `backlog_review`. Include what was done, workflow/environment issues, actions taken, and follow-up task IDs. Check that README, contracts, task state and version history agree; fix justified environment/skill/hook issues and rerun affected checks.
5. Run `ai_history.py finalize <id>` to reconcile actual children/history and validate metadata. Finalized means ready for local release gates, not published. Snapshot promotion accepts an active finalized snapshot; mark it done only after authorized external publication establishes its actual artifact evidence.
6. Stage the task finalization and obtain [the required review receipts](COMMITS.md), following the [review and verification policy](../../AGENTS.md#review-and-verification-policy). Before committing, run `ai_check.py --candidate --level snapshot` (or `minor`/`major`). Candidate checks allow only dirty Git state because the reviewed change is staged; snapshot metadata and every other enabled check must pass.

## Enabled checks

[ai_checks.json](../../script/ai_checks.json) registers extra checks. The full gate runs build, complete CTest, tooling/doc/task checks, whitespace and `publish.py --checks-only`; enabled commands add checks, and any failure blocks closure.

Screenshot tests stay disabled with an explicit reason until implemented. Platform diagnostics supplement, but never replace, actual platform evidence and recorded limitations.

Snapshot 7 requires the exact-promotion-commit MC-AI-0249 sanitizer/static-
analysis matrix before tagging; missing evidence blocks release. Linux is
analysis-only. `ai_publish.py finish` runs the local candidate gate. Pre-push
grants fast mode only to a verified two-parent snapshot promotion matching the
remote base, task trailers, and expected merge tree; other `ai-main` pushes
stay strict. `ai_publish.py tag` and tag pushes require the successful
exact-commit receipt. Packaging polls `ai-main` every 30 seconds (350-minute
cap); tag/manual dispatch verify immediately.

## Publish

Use [the version/branch procedure](../VERSION_CONVENTION.md). Finalization and `ai-main` promotion commits use the aggregate task and need Luna review. Final minor/major publication also needs Terra review. Validate the merge candidate before committing; push the exact promotion commit for hosted analysis, then require strict checks and its exact receipt before tagging. Failed validation blocks promotion; never fabricate receipts or release dates.

Local commits/tags and remote publication are distinct. A request to create planning tasks does not authorize either release execution or remote mutation. A later release request authorizes its scoped local workflow; push/releases need the requested external authority. After actual publication, make an `ai-dev` metadata ledger commit with the aggregate `Task-ID`, real published refs/artifacts, `resolved_at`, and `done` status. The snapshot heading date is bound to the finalized source commit's timestamp in `Europe/Belgrade` and stays fixed through promotion. The tag date is derived from the immutable promotion commit's timestamp in that timezone; `resolved_at` records the actual publication date. This record follows the immutable tag and is excluded from the next snapshot's basic-task collection. Snapshot 7 uses the same ledger record; its minor remains active and unfinalized until requested feedback, without creating another snapshot.

Release preparation is staged: establish the immutable source identity, run affected-module checks per task, and run the one full integration gate at the batch boundary. Once the main gameplay features are usable, launch the normal playable macOS candidate for user inspection before platform-CI work. Mac approval may unlock the next snapshot's implementation in a separate task while the current snapshot finishes its Windows/Android checks and artifact evidence; it does not authorize publication, replace required evidence, or close the current release.

Agents publish only `ai-dev` and `ai-main`; `dev` and `main` remain untouched. Canonical tags are single-use and never CI transport. Exact `ai-dev` commits may be dispatched.

Late assets may be attached with exact source, provenance and hashes. An authorized incomplete prerelease lists every pending platform, asset or check and remains active. Never silently replace accepted macOS bytes, move an immutable tag, or misstate source identity. If an immutable tag locks source that cannot satisfy the release contract, promote the corrected source and create the next unused `-rN` revision tag for that same snapshot; retain the original release as a superseded prerelease. Each revision tag is immutable and may be published only once. Shared/gameplay changes invalidate affected acceptance. `done` waits for all required artifacts and runtime evidence; missing evidence is never inferred.

After each authorized snapshot publication, verify the exact tag/signing and asset checksums against the accepted candidate. Replay the macOS artifact only when the published bytes differ, diagnosis requires it, or the user asks; the pre-publication hands-on check remains the normal play checkpoint. Do not auto-publish a minor or invent another snapshot during the post-Snapshot-7 feedback hold.
