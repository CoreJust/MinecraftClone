---
name: plan-for
description: Use when the user asks to create a project backlog task from a description.
---

# plan-for

Run `python3 script/ai_plan.py plan-for '<description>'` with the description as one safely quoted argument. This allocates the next numeric task ID and creates an unassigned basic task. Inspect relevant existing context, then use `script/ai_tasks.py update` to replace provisional metadata with concrete motivation, context, complexity, importance and observable acceptance. Keep unknowns explicit. Report the numeric ID, short scope and task link. Creating a plan does not implement or commit it.
