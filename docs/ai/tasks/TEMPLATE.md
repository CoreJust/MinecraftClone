# Task authoring template

Use `ai_tasks.py add --help`; canonical records generate `tasks/<ID>.md` automatically. Do not hand-edit generated task documents.

| Required metadata | Fill before work |
|---|---|
| Motivation | Why this change is needed |
| Pre-existing context | Behavior, evidence, assumptions, inherited work |
| Importance / complexity | P0–P3 / low–high |
| Creation date | Actual ISO date |
| Level / parent | basic→snapshot→minor→major |
| Plan / acceptance | Scope, observable result and verification |

On resolution, fill the actual resolution date, resolution changes and evidence. Aggregates preserve plans and list actual children plus product changes before code changes. Minor/major closure adds docs/environment review, hindsight and backlog actions. See [policy](../WORKFLOW.md) and [commit protocol](../COMMITS.md).
