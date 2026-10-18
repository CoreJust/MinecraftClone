# Snapshot and version closure

Planning tasks are not releases; see the [task hierarchy](tasks/MC-AI-0034.md).

| Task level | Children | Completion gates |
|---|---|---|
| Basic | — | Task metadata, minimal automatic checks, Luna/high commit review |
| Snapshot | Every basic task since previous snapshot boundary | Full build/test suite, all enabled code checks, enabled additional tests, product/code change lists, Luna/high review |
| Minor | All included snapshots | Snapshot gates, final pre-publication Terra/high review of accumulated changes after the user-feedback hold, docs/environment sanity, hindsight and backlog maintenance |
| Major | All included minors | Minor gates across the entire major range, integration/compatibility acceptance, major hindsight |

## Finalize the plan

At release start, initialize the managed report folder and ordered feature
manifest. Follow [release reporting](RELEASE_REPORTS.md) for user-visible
three-hour PDFs, fourth-period twelve-hour consolidation, and evidence-checked
cleanup only after verified full delivery. This applies to future snapshots
and versions.

1. Keep `baseline_commit` immutable. If AI tags are absent, fetch without force using `git fetch --no-tags origin 'refs/tags/ai/*:refs/tags/ai/*'`; run `ai_history.py collect <id>` and verify every commit and promised scope.
2. Complete basic tasks before snapshots, snapshots before minors, and minors before majors. Add discoveries; never remove unfinished work to pass closure.
3. Record `product_changes`, then `code_changes` (use “No product behavior changed” for tooling-only work). Active snapshots record local gates and `resolution_changes`, not a release date; only a post-publication `done` ledger records actual `resolved_at` and artifact evidence.
4. Minor/major tasks record `docs_review`, `environment_review`, `retrospective`, and `backlog_review` with observed issues, repairs and follow-up IDs. Reconcile and verify justified README, contract, task and history repairs.
5. Run `ai_history.py finalize <id>` to reconcile children/history. Finalized means ready for gates, not published. Mark a snapshot done only after authorized publication supplies artifact evidence.
6. Stage finalization and obtain [required review receipts](COMMITS.md) under the [review policy](../../AGENTS.md#review-and-verification-policy). Before committing, run `ai_check.py --candidate --level snapshot` (or `minor`/`major`). Only staged Git dirt is allowed; metadata and all other enabled checks must pass.

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

Use the [version/branch procedure](../VERSION_CONVENTION.md). Finalization and `ai-main` promotion commits use the aggregate task and Luna review. Validate the merge candidate, local qualification and promotion authenticity before tagging; failures block promotion.

Planning tasks do not authorize release execution or remote mutation. A later release request authorizes only its scoped local workflow; push or release needs requested external authority. Commits/tags do not authorize remote publication. After authorized publication, make an `ai-dev` ledger commit with aggregate `Task-ID`, actual refs/artifacts, `resolved_at` and `done`. The snapshot date is fixed from the finalized source commit; the tag date uses the promotion commit, both in `Europe/Belgrade`. Keep the ledger after the immutable tag, outside the next snapshot's task set. Snapshot 7's minor remains active until requested feedback; do not create S8.

Run focused checks per task and one full integration gate per batch. Launch the normal macOS candidate for hands-on acceptance before final qualification. Approval does not authorize publication, replace evidence or close the release. S7's minor remains held; do not start S8.

Agents publish only `ai-dev` and `ai-main`; `dev` and `main` remain untouched. Canonical tags are single-use and never CI transport. Exact `ai-dev` commits may be dispatched.

Attach late assets only with exact source, provenance and hashes. An authorized incomplete prerelease lists every pending platform/asset/check and stays active. Never replace accepted Mac bytes, move an immutable tag or misstate source identity. If tagged source cannot satisfy the contract, promote a correction under the next unused immutable `-rN` tag and retain the original as superseded. Shared/gameplay changes invalidate affected acceptance; `done` requires every artifact and runtime receipt.

After publication, verify exact tag/signing and asset hashes. Replay Mac only for changed bytes, diagnosis or user request; the normal play checkpoint is pre-publication. Do not auto-publish the minor or create another snapshot during the S7 feedback hold.
