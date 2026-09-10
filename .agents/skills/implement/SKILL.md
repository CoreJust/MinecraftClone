---
name: implement
description: Use when the user asks to implement and commit one project task or a described change.
---

# implement

1. Read [the task loop](../../../docs/ai/WORKFLOW.md) and inspect Git status. Run `python3 script/ai_plan.py implement '<task-id-or-description>' --prepare`. A numeric argument selects a task; a description creates one. The command attaches it to the current snapshot and marks it active. It only prepares task state; continue through implementation and commit.
2. Resolve provisional metadata and acceptance before coding. Respect existing assignment/dependency failures; do not move another snapshot's task silently. Use Luna/high for well-specified implementation and the [routing policy](../../../docs/ai/MODELS.md) when harder implementation or unresolved design warrants Terra/Sol/Astra.
3. Implement the task, run focused tests, update owning docs and task resolution date/changes/evidence. Preserve unrelated changes and stage exact task paths.
4. Follow [the commit protocol](../../../docs/ai/COMMITS.md): minimal checks, actual Luna/high review of the staged candidate, receipt, and exactly one Task-ID trailer. Fix failures and renew stale receipts. Commit locally and verify the resulting task/hash association with `ai_history.py show`.
5. Report the commit, task, checks and remaining limitations. Push only with external publication authorization. If already resolved, report its existing commits; do not invent another change.
