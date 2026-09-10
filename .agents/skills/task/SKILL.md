---
name: task
description: Use when the user asks for project task information by numeric ID.
---

# task

Run `python3 script/ai_plan.py task <task-id>` from the repository root. Task IDs are sequential numbers; `31` selects the record stored as `MC-AI-0031`. Print the returned task information. For exact Git backlinks, run `python3 script/ai_history.py show MC-AI-0031` with the resolved ID and read its returned file. This is a read-only lookup.
