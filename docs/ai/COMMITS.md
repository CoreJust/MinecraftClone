# Commit protocol

Every new AI-line commit, including a merge or task-metadata change, references exactly one task:

```text
Describe the change

Task-ID: MC-AI-0031
```

The task must exist in the staged backlog. Stage only that task's work and required parent/traceability updates. Preserve other staged or untracked work. For basic tasks, the gate checks an exact temporary export of the reviewed index; unrelated working-tree changes do not enter that check. Aggregate candidate/build checks still require an exact source checkout.

## Review once, bind it to the candidate

1. Before final staging, run focused tests, formatters, and source-policy checks. Resolve correctness failures while the working tree is still easy to inspect.
2. Stage the final task change, including regenerated guides/task documents.
3. Run `python3 script/ai_commit.py candidate MC-AI-0031`. Give its immutable `head`, `tree`, task/level/baseline/merge-head fields and the staged diff to one fresh reviewer using the active selected model and effort. The default review route follows the [review and verification policy](../../AGENTS.md#review-and-verification-policy); an explicitly selected task profile controls its model and effort. Read the actual staged contents, not unstaged files.
4. The reviewer returns an approved JSON report containing the candidate fields plus `review_role: task`, the actual `model`, the selected `effort`, `verdict: approved`, and a concise nonempty `evidence` receipt. Run `ai_commit.py record-review <report.json>`. Keep the batch bounded to genuine correctness findings; if a correction changes the candidate, send only that delta to the same reviewer and renew the receipt. Defer non-blocking cosmetic suggestions.
5. At the final aggregate gate—not during routine minor development, CI candidates, or snapshot delivery—also record the accumulated-change report required by the [review and verification policy](../../AGENTS.md#review-and-verification-policy). For a minor, this is after the requested user-feedback hold and immediately before publication; for a major, follow its aggregate gate. Use `review_role: aggregate` with the actual model and selected effort; all candidate identity fields must match.
6. Commit with the trailer. The commit hook owns the development gate; do not run a duplicate precommit wrapper. Hooks validate the review receipts, run the appropriate checks, and reject a missing/mismatched task, stale candidate, or failed gate.

Receipts accept `gpt-6-luna`, `gpt-6-sol`, `gpt-6.1-sol`, and `gpt-6-astra`. All accept `low`, `medium`, `high`, `xhigh`, and `max`; all except Luna also accept `ultra`. New-model receipts require an explicit role. Older receipts without `review_role` retain Luna → task and Sol → aggregate inference, including their original local paths and task selector. Explicit roles take precedence over that legacy inference. A task receipt selects the current task and cannot satisfy the aggregate review, or vice versa.

Receipts are local attestations of a real review, not cryptographic proof of who reviewed. Never fabricate one. Each role has one reusable review result for an unchanged candidate; receipt reuse is valid only when the checker confirms its inputs are unchanged. Hooks make no paid model calls and do not loop on failed reviews. A changed HEAD, pending merge parent, index tree, task, or baseline invalidates the receipt. Task review covers the task commit; aggregate review covers the accumulated version changes. Each task produces one commit; later work belongs to a new task, and published history is never silently rewritten.

## Bidirectional links without recursive commits

A commit cannot contain its own final hash: inserting that hash changes the commit. Tracked task documents therefore store the stable `Task-ID` selector and any explicitly imported legacy references. `ai_history.py show <id>` generates `build/ai-tasks/<id>.md` with the exact matching hashes and clickable GitHub commit links. The post-commit hook refreshes these pages automatically, without editing tracked files or making another commit.

After cloning or fetching, run `python3 script/ai_history.py refresh` to rebuild local backlinks from available Git history. Unpushed hashes resolve locally; GitHub links become available only after authorized publication. Commit messages identify the corresponding tracked task document under `docs/ai/tasks/` by its ID.

The inherited history predates trailers. Its explicit imported references are assigned to baseline task `MC-AI-0001`; this is traceability, not a claim that those changes were implemented or verified by the new workflow. New commits must use the trailer.
