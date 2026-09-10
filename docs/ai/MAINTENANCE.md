# Documentation maintenance

Keep facts at one owner. Source and tests define current behavior; [product roadmap](../ROADMAP.md) defines intent; [backlog](backlog.json) defines work state. Guides explain contracts and navigation. Historical releases and design explorations are not current implementation specifications.

| Changed fact | Update in the same task |
|---|---|
| Ownership, public contract, lifetime, thread/error behavior | Owning code guide from [modules.json](../code/modules.json) |
| Source/test/build/tool file added, removed, or changed | Review its guide, then regenerate index and source state |
| CLI, build prerequisite, hook, skill, model setting | [build guide](../BUILD.md), [automation](AUTOMATION.md), or root instructions |
| New issue, dependency, blocked work, acceptance result | Backlog through `ai_tasks.py`; generate each task document |
| Product intent | Canonical roadmap only when the user authorizes the changed scope |
| Snapshot actually released | Version history with the real date and verification evidence |

## Freshness gate

`ai_docs.py check` requires each source/test/build/tool file to have exactly one module owner, validates local documentation links and word limits, and compares the generated index and per-module source/guide hashes. New paths cannot silently disappear from the map. It does not parse C++ semantics or prove prose true.

After a change, read the affected guide against the changed source. Update it only where facts changed; unchanged contracts need no filler sentence. Run `python3 script/ai_docs.py refresh` to acknowledge that comparison, then `check`. Never put `refresh` in CI or a hook: automatic acknowledgment would hide stale prose.

Generated [INDEX.md](../code/INDEX.md) and [source_state.json](../code/source_state.json) are mechanical navigation/freshness artifacts. Do not load them wholesale in ordinary sessions; search an exact symbol/path and open one guide. Future AST/API extraction is optional only if navigation metrics show a need.

## Context budget

Root instructions stay below 650 words; each new guide below 900; the complete roadmap below 1,400. Store large file inventories in generated output, not prose. Describe ownership, invariants, extension points, and test gaps rather than repeating signatures. Link to code for exact declarations.

At each snapshot, remove stale guidance, deduplicate facts, archive completed task narratives if they obstruct retrieval, and keep the stable backlog record and generated task specification. At minor/major closure, record docs/environment review, hindsight, and backlog reorganization in the aggregate task. Review only relevant vendor docs when an installed tool changes. Track elapsed time, repeated failures, and checks run in task evidence; adjust model/reasoning choices from observed work rather than claiming token savings without measurements.
