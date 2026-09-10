---
name: major
description: Use when the user requests information about a project major version.
---

# major

Run `python3 script/ai_plan.py major [version]`. Omit the version to select the current in-development major from `docs/ai/current.json`. Print its plan, status and child minors. A missing current selection is an error. This is a read-only lookup.
