# Model routing and execution cost

Use the least expensive route that can make the task's decisions reliably. All four routes use **high** reasoning. This is the owner's routing policy, not a claim of measured model superiority or token savings. For comparable work, the efficiency target is no more than 50% of the S4 RED elapsed time and no more than one-third of its usage; this remains a planning target until comparable evidence establishes a result.

| Route | Model | Work in this project | Output to the next step |
|---|---|---|---|
| `astra` | `gpt-6-astra` | Architecture/product decisions, conflicting requirements, milestone orchestration, README/agent-policy/skill design | Short decision, invariants, scope, task split |
| `sol` | `gpt-5.6-sol` | Precise unresolved local design, subsystem boundaries, complex isolated algorithms, component orchestration | Implementation contract, owned paths, acceptance/tests |
| `terra` | `gpt-5.6-terra` | Harder implementation, debugging, or a Luna escalation; important short reviews | Patch, test evidence, unresolved facts |
| `luna` | `gpt-5.6-luna` | Well-specified implementation, tests, tooling, metadata, reviews, mechanical migrations, and efficient handoffs | Mechanical output or bounded findings, checker result |

Default: Luna/high for well-specified implementation, tests, tooling, metadata, and reviews. Escalate harder implementation or repeated predicate failure to Terra/high, precise unresolved design to Sol/high, and architecture/product decisions to Astra/high. The backlog's `route` records the intended worker; `owner` records who writes. Choose by remaining decisions, not file count.

## One planning pass, many cheap executions

For a new milestone, Astra is useful only if it contains a major unresolved design or product decision. It writes a compact decision and exits. Sol can divide a complex subsystem into independently testable tasks. Terra or Luna then implements each task directly; straightforward work skips planning delegation entirely.

Packet budget: target 300 words, at most 600. An automated packet includes task ID, route, exact outcome, owned paths, contracts, acceptance command, blocker, current evidence, and an honest comparison of the next five ordinary tasks after efficiency is enabled, including partial or unknown results. Link evidence; omit chat history and full source. Do not invent tasks or change the S3 pause scope. Read one code guide and `ai_tasks.py show <id>` instead of the whole backlog/index. High-level orchestrators consume completion receipts, not every intermediate tool result.

Do not run every change through all four models. Finish clean tasks on Luna. Spawn only independent work that repays handoff/context cost; give writers disjoint files, and serialize task metadata updates. One coordinator owns integration.

## Usage accounting

Record actual provider input, cached-input, and output tokens when available; never infer or invent missing shares. Measure elapsed time and usage only from comparable existing summaries; report missing fields as unavailable and never turn the efficiency target into an invented percentage or achieved result.

## Escalation and review

If a worker fails the same acceptance predicate twice, stop retrying blindly. Send the failing test, smallest relevant diff, attempted approaches, and one precise question to the next suitable route. Escalate immediately for a newly discovered major contract decision. After the decision, return implementation to the original cheap route. Do not automatically ask Astra to review every completion.

Deterministic checks remain the main debugging tool and run before review. Start from the immutable candidate, failing command, raw logs, and smallest relevant diff; do not guess from prose. Review timing, staged-tree identity, and testing priorities follow the [review and verification policy](../../AGENTS.md#review-and-verification-policy). Reuse still-valid evidence and avoid extra review rounds beyond [the commit policy](COMMITS.md).

Examples: chunk coordinate design → Astra once; meshing contract → Sol if unresolved; meshing code → Terra; a specified block registry → Luna. Malformed-message regression → Terra; a wire-format compatibility decision → Astra or Sol according to its reach.

## Launch and verify

```sh
python3 script/ai_run.py --dry-run luna -- exec 'Implement MC-AI-0004 within its acceptance criteria'
python3 script/ai_run.py sol
```

The launcher passes explicit model/high flags to Codex and starts in the repository root. It does not schedule runs, bypass permissions, or purchase usage. `.codex/config.toml` supplies Luna/high for ordinary new sessions in this trusted project. App/session overrides can win; a coordinator must explicitly set the intended model when spawning a worker. These presets do not change this already-running task's model.

Use the launcher instead of installing global profiles. In the inspected Codex CLI, `--profile` reads user-level profile files; explicit flags keep routing project-local. If a model is unavailable, report that and select the nearest available route without claiming the configured model ran.

At a milestone boundary, compare task elapsed time, failed-check retries, escalation count, and model usage when available. Keep the lowest-cost route that still meets acceptance. Record only a short receipt; do not create a second analytics project.
