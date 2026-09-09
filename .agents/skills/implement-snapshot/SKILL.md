---
name: implement-snapshot
description: Use when the user asks to fully implement and close the current project snapshot.
---

# implement-snapshot

1. Run `python3 script/ai_plan.py implement-snapshot`. It requires a selected in-development snapshot and prints its pending work; it does not execute implementation. Read [aggregate execution](../../../docs/ai/SKILLS.md#aggregate-execution) and [release gates](../../../docs/ai/RELEASES.md).
2. Fulfil the current plan through all its basic tasks, resolving dependencies in order. Use the `implement` skill for basic tasks and the corresponding aggregate skill for child versions. Recompute remaining work after each child; record new findings as tasks. Do not declare completion while planned scope is missing.
3. Finalize actual child/change lists, run the complete snapshot checks and publishing workflow, and commit the reviewed promotion into `ai-main`. Minor/major closure also needs Terra/high review, docs/environment sanity, hindsight and backlog maintenance. Local completion and remote publication must be reported separately.
4. After successful closure, ask the user for the next snapshot's identifier and scope. Wait for their answer, then create its planned task and select it through `ai_plan.py select`. A nested invocation defers this question to the owning aggregate orchestrator, which handles the same next-child decision. Never guess a successor or reopen the completed task.
