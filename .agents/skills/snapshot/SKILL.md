---
name: snapshot
description: Use when the user requests information about a project snapshot.
---

# snapshot

Run `python3 script/ai_plan.py snapshot [snapshot-identifier]`. Omit the identifier to select the current in-development snapshot from `docs/ai/current.json`. Print its plan, status and children. A missing current selection is an error; do not invent a snapshot. This is a read-only lookup.
