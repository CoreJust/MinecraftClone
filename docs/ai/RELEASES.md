# Snapshot and version closure

Planning tasks are not releases; see the [task hierarchy](tasks/MC-AI-0034.md).

| Task level | Children | Completion gates |
|---|---|---|
| Basic | — | Task metadata, minimal automatic checks, Luna/high commit review |
| Snapshot | Every basic task since previous snapshot boundary | Full build/test suite, all enabled code checks, enabled additional tests, product/code change lists, Luna/high review |
| Minor | All included snapshots | Snapshot gates, final pre-publication Terra/high review of accumulated changes after the user-feedback hold, docs/environment sanity, hindsight and backlog maintenance |
| Major | All included minors | Minor gates across the entire major range, integration/compatibility acceptance, major hindsight |

## Finalize the plan

1. Keep `baseline_commit` immutable. If prior AI tags are absent, fetch them without force using `git fetch --no-tags origin 'refs/tags/ai/*:refs/tags/ai/*'`; then run `ai_history.py collect <id>` and confirm the graph accounts for every commit and promised scope.
2. Complete basic children before their snapshot; complete snapshots before the minor and minors before the major. Add newly discovered tasks; do not remove unfinished planned work to make closure pass.
3. Fill `product_changes`, then `code_changes`. Use a truthful “No product behavior changed” for tooling-only work. An active snapshot records local gate evidence and `resolution_changes` without a release date; only the post-publication `done` ledger records the actual `resolved_at` date and external artifact evidence.
4. Minor/major tasks fill `docs_review`, `environment_review`, `retrospective`, and `backlog_review`: observed issues, repairs and follow-up IDs. Reconcile README, contracts, task state and history; verify justified repairs.
5. Run `ai_history.py finalize <id>` to reconcile actual children/history and validate metadata. Finalized means ready for local release gates, not published. Snapshot promotion accepts an active finalized snapshot; mark it done only after authorized external publication establishes its actual artifact evidence.
6. Stage the task finalization and obtain [the required review receipts](COMMITS.md), following the [review and verification policy](../../AGENTS.md#review-and-verification-policy). Before committing, run `ai_check.py --candidate --level snapshot` (or `minor`/`major`). Candidate checks allow only dirty Git state because the reviewed change is staged; snapshot metadata and every other enabled check must pass.

## Enabled checks

[ai_checks.json](../../script/ai_checks.json) registers extra checks. The full gate runs build, complete CTest, tooling/doc/task checks, whitespace and `publish.py --checks-only`; enabled commands add checks, and any failure blocks closure.

Screenshot tests stay disabled with an explicit reason until implemented. Platform diagnostics supplement, but never replace, actual platform evidence and recorded limitations.

Snapshot 7 qualification is local-first under MC-AI-0434. Windows and macOS
remain the primary targets. Required tests, sanitizers, static analysis and
runtime checks run locally; missing, failed or stale evidence blocks release.
Linux is analysis-only, not a supported game target. Android requires local
tests, basic runtime evidence and the development APK; its hosted HWASan and
analyzer gates are suspended by the owner's revised scope.

Qualification evidence binds the relevant source, dependency, configuration,
tool and command inputs. Documentation or task-ledger changes alone do not
invalidate native evidence. Release authenticity independently binds the exact
source commit, promotion parents/tree, immutable tag and artifact hashes.
Build packages locally and upload them; GitHub does not qualify the release or
run automatic jobs. Manual artifact builds are an optional fallback, not a gate.
Never relabel old hosted evidence as a local
pass or treat an unavailable tool as a successful check.

## Publish

Use [the version/branch procedure](../VERSION_CONVENTION.md). Finalization and `ai-main` promotion commits use the aggregate task and need Luna review. Validate the merge candidate before committing and require local qualification plus exact promotion authenticity before tagging. Failed validation blocks promotion; never fabricate receipts or release dates.

Local commits/tags and remote publication are distinct. A request to create planning tasks does not authorize either release execution or remote mutation. A later release request authorizes its scoped local workflow; push/releases need the requested external authority. After actual publication, make an `ai-dev` metadata ledger commit with the aggregate `Task-ID`, real published refs/artifacts, `resolved_at`, and `done` status. The snapshot heading date is bound to the finalized source commit's timestamp in `Europe/Belgrade` and stays fixed through promotion. The tag date is derived from the immutable promotion commit's timestamp in that timezone; `resolved_at` records the actual publication date. This record follows the immutable tag and is excluded from the next snapshot's basic-task collection. Snapshot 7 uses the same ledger record; its minor remains active and unfinalized until requested feedback, without creating another snapshot.

Run focused checks per task and one full integration gate per batch. Launch the normal macOS candidate for hands-on acceptance before final qualification. Approval does not authorize publication, replace evidence or close the release. S7's minor remains held; do not start S8.

Agents publish only `ai-dev` and `ai-main`; `dev` and `main` remain untouched. Canonical tags are single-use and never CI transport. Exact `ai-dev` commits may be dispatched.

Late assets may be attached with exact source, provenance and hashes. An authorized incomplete prerelease lists every pending platform, asset or check and remains active. Never silently replace accepted macOS bytes, move an immutable tag, or misstate source identity. If an immutable tag locks source that cannot satisfy the release contract, promote the corrected source and create the next unused `-rN` revision tag for that same snapshot; retain the original release as a superseded prerelease. Each revision tag is immutable and may be published only once. Shared/gameplay changes invalidate affected acceptance. `done` waits for all required artifacts and runtime evidence; missing evidence is never inferred.

After each authorized snapshot publication, verify the exact tag/signing and asset checksums against the accepted candidate. Replay the macOS artifact only when the published bytes differ, diagnosis requires it, or the user asks; the pre-publication hands-on check remains the normal play checkpoint. Do not auto-publish a minor or invent another snapshot during the post-Snapshot-7 feedback hold.
