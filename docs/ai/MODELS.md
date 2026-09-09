# Model routing and execution cost

Use the least expensive route that can make the task's decisions reliably. All four routes use **high** reasoning. This is the owner's routing policy, not a claim of measured model superiority or token savings.

| Route | Model | Work in this project | Output to the next step |
|---|---|---|---|
| `astra` | `gpt-6-astra` | World/chunk architecture, major protocol/save decisions, conflicting product requirements, milestone orchestration, README/agent-policy/skill design | Short decision, invariants, scope, task split |
| `sol` | `gpt-5.6-sol` | Local subsystem design, complex isolated algorithms, component orchestration, code guides, ordinary design decisions | Implementation contract, owned paths, acceptance/tests |
| `terra` | `gpt-5.6-terra` | Most feature/bug implementation, tests, debugging, short reviews of important changes | Patch, test evidence, unresolved facts |
| `luna` | `gpt-5.6-luna` | Explicit mechanical migrations, repetitive registries/content, simple code research, boilerplate, broad routine review, clearly specified verifiable tasks | Mechanical output or bounded findings, checker result |

Default: Terra/high. The backlog's `route` records the intended worker. Its `owner` records who currently writes. Choose a route by remaining decisions, not file count: a thousand-line registry with a generator/check is Luna work; a twenty-line ownership change can need Sol design and Terra implementation.

## One planning pass, many cheap executions

For a new milestone, Astra is useful only if it contains a major unresolved design or product decision. It writes a compact decision and exits. Sol can divide a complex subsystem into independently testable tasks. Terra or Luna then implements each task directly; straightforward work skips planning delegation entirely.

Packet budget: target 300 words, at most 600. Include task ID, route, exact requested outcome, owned paths, contracts, acceptance command, and known blocker. Link evidence; omit chat history and full source. Read one code guide and `ai_tasks.py show <id>` instead of the whole backlog/index. High-level orchestrators consume completion receipts, not every intermediate tool result.

Do not run every change through all four models. Finish clean tasks on their initial route. Spawn only independent work that repays handoff/context cost; give writers disjoint files, and serialize task metadata updates. One coordinator owns integration.

## Escalation and review

If a worker fails the same acceptance predicate twice, stop retrying blindly. Send the failing test, smallest relevant diff, attempted approaches, and one precise question to the next suitable route. Escalate immediately for a newly discovered major contract decision. After the decision, return implementation to the original cheap route. Do not automatically ask Astra to review every completion.

Deterministic checks remain the main debugging tool. Every commit also requires a short Luna/high review of the exact staged tree. Minor/major completion requires Terra/high review of the accumulated changes. Use Sol/Astra only for unresolved architecture or important decisions. Reuse still-valid evidence and avoid extra review rounds beyond [the commit policy](COMMITS.md).

Examples: chunk coordinate design → Astra once; meshing contract → Sol if unresolved; meshing code → Terra; a specified block registry → Luna. Malformed-message regression → Terra; a wire-format compatibility decision → Astra or Sol according to its reach.

## Launch and verify

```sh
python3 script/ai_run.py --dry-run terra -- exec 'Implement MC-AI-0004 within its acceptance criteria'
python3 script/ai_run.py sol
```

The launcher passes explicit model/high flags to Codex and starts in the repository root. It does not schedule runs, bypass permissions, or purchase usage. `.codex/config.toml` supplies Terra/high for ordinary new sessions in a trusted project. App/session overrides can win; a coordinator must explicitly set the intended model when spawning a worker. These presets do not change this already-running task's model.

Use the launcher instead of installing global profiles. In the inspected Codex CLI, `--profile` reads user-level profile files; explicit flags keep routing project-local. If a model is unavailable, report that and select the nearest available route without claiming the configured model ran.

At a milestone boundary, compare task elapsed time, failed-check retries, escalation count, and model usage when available. Keep the lowest-cost route that still meets acceptance. Record only a short receipt; do not create a second analytics project.
