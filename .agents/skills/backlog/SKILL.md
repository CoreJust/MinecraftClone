---
name: backlog
description: Use when the user requests the project backlog of unassigned tasks.
---

# backlog

Run `python3 script/ai_plan.py backlog` and print the result. The backlog contains unfinished basic tasks with no snapshot parent. Assigned work belongs to its snapshot and is excluded. `docs/ai/BACKLOG.md` is the complete task registry; it is not this filtered view. This command is read-only.
