# Commit protocol

Every new AI-line commit, including a merge or task-metadata change, references exactly one task:

```text
Describe the change

Task-ID: MC-AI-0031
```

The task must exist in the staged backlog. Stage only that task's work and required parent/traceability updates. Preserve other staged or untracked work; the gate rejects a working tree that differs from staged governed files. Use an isolated task checkout when another task prevents an exact check.

## Review once, bind it to the candidate

1. Stage the final task change, including regenerated guides/task documents.
2. Run `python3 script/ai_check.py --fast --require-index-match`.
3. Run `python3 script/ai_commit.py candidate MC-AI-0031`. Give its immutable `head`, `tree`, task/level/baseline/merge-head fields and the staged diff to a fresh **Luna/high** reviewer. Read the actual staged contents, not unstaged files.
4. The reviewer returns an approved JSON report containing the candidate fields plus `model: gpt-5.6-luna`, `effort: high`, `verdict: approved`, and a concise nonempty `evidence` receipt. Run `ai_commit.py record-review <report.json>`.
5. At the final aggregate gate—not during routine minor development, CI candidates, or snapshot delivery—also record the accumulated-change report required by the [review and verification policy](../../AGENTS.md#review-and-verification-policy). For a minor, this is after the requested user-feedback hold and immediately before publication; for a major, follow its aggregate gate. Use `model: gpt-5.6-terra`; all candidate identity fields must match.
6. Commit with the trailer. Hooks validate the review receipts, run the appropriate checks, and reject a missing/mismatched task, stale candidate, or failed gate.

Receipts are local attestations of a real review, not cryptographic proof of who reviewed. Never fabricate one. The receipt is the single reusable review result for an unchanged candidate; do not run a duplicate precommit wrapper or claim unmeasured model usage. Hooks make no paid model calls and do not loop on failed reviews. A changed HEAD, pending merge parent, index tree, task, or baseline invalidates the receipt. Review scope for Luna is the commit; Terra's version review covers the accumulated version changes.

## Bidirectional links without recursive commits

A commit cannot contain its own final hash: inserting that hash changes the commit. Tracked task documents therefore store the stable `Task-ID` selector and any explicitly imported legacy references. `ai_history.py show <id>` generates `build/ai-tasks/<id>.md` with the exact matching hashes and clickable GitHub commit links. The post-commit hook refreshes these pages automatically, without editing tracked files or making another commit.

After cloning or fetching, run `python3 script/ai_history.py refresh` to rebuild local backlinks from available Git history. Unpushed hashes resolve locally; GitHub links become available only after authorized publication. Commit messages identify the corresponding tracked task document under `docs/ai/tasks/` by its ID.

The inherited history predates trailers. Its explicit imported references are assigned to baseline task `MC-AI-0001`; this is traceability, not a claim that those changes were implemented or verified by the new workflow. New commits must use the trailer.
