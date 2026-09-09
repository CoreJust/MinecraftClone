# Project commands

Project skills live in [.agents/skills](../../.agents/skills). Reload the project session to discover them. Invoke `$task 31`, `$snapshot`, or the other names below. Their deterministic backend is `python3 script/ai_plan.py`; it performs selection and task-state changes without calling a model.

Task IDs are sequential numbers, starting at 1. The stable file/trailer spelling is `MC-AI-0031` for task 31; numbers are never recycled. `docs/ai/current.json` explicitly selects the current snapshot, minor and major. Missing or inconsistent selections fail rather than guessing from dates or task order.

| Skill | Behavior |
|---|---|
| `task <id>` | Print one task and its metadata |
| `snapshot [identifier]` | Print the selected snapshot, or the current in-development snapshot |
| `minor [version]`, `major [version]` | Print selected/current version plans and children |
| `backlog` | Print unfinished basic tasks with no snapshot assignment |
| `plan-for <description>` | Create an unassigned task; refine its provisional metadata from context |
| `implement <id or description>` | Select/create, attach to current snapshot, implement, verify, Luna-review and commit |
| `implement-snapshot` | Complete the current snapshot and its full release workflow |
| `implement-minor`, `implement-major` | Complete the current version, including its child aggregates and extra closure gates |

Lookup commands are read-only. `ai_plan.py implement ... --prepare` only prepares state; the skill must then perform the coding, validation and commit. Likewise, aggregate backend commands print an ordered work queue, not a completion claim. Descriptions remain data, passed as a single safely quoted argument.

## Aggregate execution

1. Resolve the explicitly selected aggregate and inspect its plan, children and dependencies. A snapshot implementation requires an in-development snapshot. Block on unresolved product decisions; implement all already-authorized independent work first.
2. Expand uncovered product scope into basic tasks before implementing it. Assign work to the snapshot that will deliver it. Complete dependency-ready tasks through the `implement` skill. Follow [model routing](MODELS.md); use one coordinator, short worker packets and focused checks during iteration.
3. For minors/majors, complete planned child aggregates in order. If the plan needs another snapshot/minor, ask the user for its identifier and scope, create it in advance, then select it. Nested aggregate calls return to the owner; the owner handles successor questions once. Do not create an endless sequence after the parent's plan is fulfilled.
4. Reconcile every commit and child through `ai_history.py collect` and `finalize`. Run [all tier gates](RELEASES.md), the actual snapshot metadata/artifact steps and reviewed `ai-main` promotion in [version conventions](../VERSION_CONVENTION.md). Failed checks or missing runtime evidence leave the task unresolved. Record local refs and external publication separately.
5. After successful top-level closure, ask for the next aggregate's identifier and scope, wait for the response, then create its planned task under the appropriate unresolved parent. Use `ai_tasks.py add --level ...` with the required metadata and exact preceding release baseline, followed by `ai_plan.py select <level> <id>`. If the parent is closed, first establish the next parent plan with the user. Use `ai_plan.py clear <level>` after closure or `select` to replace it; selecting a new major/minor clears incompatible child selections. never point to an invented successor.

An implementation invocation authorizes its scoped local implementation and commits. Remote pushes/releases still require external publication authority. Invocation of a lookup or planning skill does not run implementation. Task creation stores intent; resolution stores actual date, changes and evidence.
