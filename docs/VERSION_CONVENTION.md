# Versions and branches

Versions use `Epoch.Major.Minor:snapshot`. An epoch names a development stage; a major version groups mechanics; a minor version adds content. Snapshots are the smallest delivery unit, written `i(yy.mm.dd)`. Each minor release should include executables.

| Line | Integration | Snapshots | Tag prefix |
|---|---|---|---|
| Original | `dev` | `main` | `<MajorName>/` |
| AI | `ai-dev` | `ai-main` | `ai/<MajorName>/` |

`ai-dev` starts at `dev`; `ai-main` starts at `main`. They evolve separately. Task branches use `codex/ai-<task>` from `ai-dev` and return there after their gates pass. Synchronization with the original line is an explicit scoped change, never an automatic merge. Do not rewrite either line's history.

## AI snapshots and version completion

Create snapshot/minor/major tasks in advance on request. Their plans and child hierarchy live in [release tasks](ai/RELEASES.md). BasicBlocks S1 is snapshot `0.1.1:1`; the next planned targets are snapshot `0.1.1:2`, minor `0.1.1`, and major `0.1`. Planning does not publish them.

1. Complete the scoped basic tasks on `ai-dev`. Every commit has exactly one `Task-ID` trailer and passes Luna/high review plus minimal checks.
2. Finalize the aggregate from its immutable baseline: collect all task commits since the preceding snapshot boundary, reconcile planned/actual children, and record product changes before code changes. Minor/major closure adds full-range Terra/high review, docs/environment review, hindsight and backlog maintenance.
3. Stage snapshot finalization with truthful local gate evidence and change lists. Bind the snapshot heading date to the finalized source commit's timestamp interpreted in `Europe/Belgrade`; it remains fixed through promotion. Keep an active snapshot's `resolved_at` empty until external publication; a `done` task requires its actual resolution date. Obtain candidate review receipts and pass the local aggregate gate. Commit using the aggregate task ID, then require a clean tree and complete runtime acceptance. Reuse expensive checks only with matching relevant-input evidence.
4. Record the exact `ai-dev` commit. In a clean checkout, switch to `ai-main` and run `git merge --no-ff --no-commit <recorded-commit>`. Review and validate this merged index through the same task's candidate protocol. Use the full snapshot version heading as the promotion subject, including its source-bound date (for example, `EarlyDev 0.1.0:7(26.10.18)`), followed by exactly one aggregate `Task-ID` trailer. Keep existing published promotion subjects unchanged. `finish` creates the verified two-parent promotion commit; unchanged qualification inputs do not require another native test run.
5. Verify local qualification and exact promotion authenticity before `tag` creates the immutable tag under `ai/<MajorName>/<Epoch.Major.Minor>/<snapshot>_<yy.mm.dd>`, using the promotion commit's timestamp converted to `Europe/Belgrade`. Push the authorized promotion and upload locally built packages; publication starts no CI. Never rewrite an existing tag.
6. Only when remote publication is authorized, push the selected tag and publish executable assets when authorized. Then record the real refs/artifacts, resolution date, and `done` status in a separate `ai-dev` aggregate metadata ledger commit. It follows the immutable tag and does not create another snapshot. After Snapshot 7, record that ledger but leave the minor active and unfinalized until user feedback; do not invent another snapshot. Avoid `--all` and `--follow-tags`, which can collect unrelated refs.

`script/ai_publish.py` performs the local snapshot-only sequence with immutable
source hashes. Run `prepare <snapshot-task> <ai-dev-HEAD>` from clean `ai-dev`;
it validates the finalized aggregate, records the current `ai-main` promotion
base, validates local candidate evidence, and proves
the immutable inputs merge cleanly before leaving a no-commit merge on `ai-main`.
Review that merged index and record its normal
`ai_commit.py` receipt. `finish` rejects an index that differs from that
deterministic merge tree and commits the two-parent promotion. Push that exact
commit to `ai-main` for artifact builds. Run `tag` after local qualification;
it validates the same merge tree and strict release evidence before creating the
one canonical annotated AI tag only if it does not already exist. It never
pushes, changes `dev`/`main`, rewrites tags, creates a successor snapshot, or
publishes a minor/major; those actions remain separately authorized workflows.

A local task marked finalized is not a published release. Review/check failures block committing or promotion; they do not justify altering product requirements or inventing release evidence. Original-line history is preserved. The non-check legacy publisher below remains outside this AI workflow.

## Existing publisher

`python3 publish.py '<MajorName>:<MinorName>' '<Epoch.Major.Minor>:<i>' --checks-only` remains the source/style/version check. Its non-check path accepts only `dev` and currently merges without switching to `main`; do not use it to promote AI snapshots. The explicit procedure above is the AI release path until a separately tested publisher change is implemented.

Original tags/history keep their names. AI tags use a namespace so equal snapshot numbers cannot collide. The roadmap's transition labels are tentative; reconcile inconsistent epoch/stage naming as a recorded product decision before changing versions.
