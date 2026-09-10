---
name: minor
description: Use when the user requests information about a project minor version.
---

# minor

Run `python3 script/ai_plan.py minor [version]`. Omit the version to select the current in-development minor from `docs/ai/current.json`. Print its plan, status and child snapshots. A missing current selection is an error. This is a read-only lookup.
