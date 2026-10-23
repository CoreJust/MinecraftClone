# Validation selector

[`script/ai_validation.py`](../../script/ai_validation.py) selects checks from
staged, unstaged, and untracked paths. The default `routine` lane runs all
small checks and affected medium checks, and reports affected large checks for
the release boundary. It prints selected paths, checks, and deferred work before
running. Unknown changed paths, unclassified configured CTest rows, missing
required consumers, or incomplete source-to-consumer coverage fail closed.

Run from the repository root. The first command plans; `--run` executes. Use
`--paths path/one path/two` to inspect those paths instead of Git discovery; it
does not describe the rest of the worktree.

```sh
python3 script/ai_validation.py
python3 script/ai_validation.py --run
python3 script/ai_validation.py --lane major --run
python3 script/ai_validation.py --lane full --run
```

## Inventory and configured graph

The checked-in [inventory](../../script/validation_inventory.json) defines
changed-path rules, source-module dependencies, test classes, release-check
owners, commands, and budgets. `fast-code` delegates documentation, backlog,
tooling, and whitespace checks to `ai_check.py --fast`. Release-check IDs and
their snapshot/minor/major levels must exactly match every enabled entry in
[`ai_checks.json`](../../script/ai_checks.json); missing, extra, or misclassified
checks fail closed.

Native CTest rows come from `ctest --show-only=json-v1` in the configured
`build/debug` tree. Each row needs a CMake source classification or exact
registration. The graph must belong to this checkout and use Ninja. Missing or
stale configuration, an absent changed test, or an unmapped row stops
selection; run `cmake --preset debug` and replan. Native-wide changes, such as
CMake or dependency manifests, select every native module and require a fresh
graph.

The last inspected graph had 868 rows; this is a historical count, not current
coverage. Source and CMake registrations have changed. The inventory requires
`MinecraftClone.ColumnMapCpuConsumer`; that row must exist in a newly captured
graph before selection can pass. Refresh the graph after source freeze, and do
not use the stale capture for release qualification. Its optional `map.vert`
and `map.frag` shader registrations did not qualify the CPU consumer.

Historical class totals (868-row capture only):

| Class | Rows | Class | Rows |
|---|---:|---|---:|
| Small core | 249 | Small shaders | 9 |
| Medium client | 310 | Medium Android | 13 |
| Medium acceptance | 108 | Medium generation | 23 |
| Medium integration | 148 | Medium tools | 1 |
| Large acceptance | 1 | Large integration | 6 |

Classify from source, tags, and the inventory, not directory names. For
example, the six-seed atlas pipeline is integration-large despite its
`tests/core` source; the single-seed reference suite is integration-medium.
`TerrainWorldMap.Cli` exercises a real one-seed regional export and six-seed
atlas export, builds `mc_world_map`, and binds its executable hash into the
CTest receipt. GPU, acceptance, and golden labels promote tests to large.

The CPU map consumer is renderer-free `mc_game_core` → `mc_column_replay` →
`mc_column_map` (`MC_BUILD_COLUMN_MAP=ON`). Changes to these sources or the
map client, presentation, replay, or CLI select client, acceptance, and
integration consumers. The medium `MinecraftClone.ColumnMapCpuConsumer` check
configures with client off, map on, and tests off; builds `mc_column_map`; and
requires its CLI to reject a zero-duration run with the documented validation
error. It reuses
`build/debug/tests/column-map-cpu-consumer/` and retains build/run logs and
configure diagnostics. The normal build also selects and hashes
`mc_column_map`; this check is separate from live replay acceptance.

The height-tile worker-pool test is integration-medium because it exercises
prepared-world preview and materialization. The inventory records module
dependency edges; they select consumers but do not change suite size. Android
Gradle paths are not in the native inventory and fail closed. CTest Android
unit tests do not prove APK, emulator, or device acceptance.

## Lanes and receipts

Budgets come from the [iteration workflow](decisions/project_iteration_workflow.md).
They are limits, not proof that lanes currently meet them. The configured graph
and complete lane timings are stale or unqualified; refresh and measure after
the source/build configuration is frozen without dropping enabled checks.

| Lane | Selection | Mac wall-clock maximum |
|---|---|---:|
| Small | All small CTest rows and fast checks | 3 minutes |
| Medium | All medium CTest rows and checks | 12 minutes |
| Routine | Small plus affected medium | 15 minutes |
| Major | Small plus all medium | 15 minutes |
| Large | All configured large CTest rows | 45 minutes |
| Full | Small, medium, large, and required release gates | 60 minutes |

Plan construction, input/prerequisite hashing, and selected native builds count
against lane and aggregate budgets; builds run before CTest. On failure, timeout,
or budget exhaustion, the runner records remaining checks as `UNRUN` instead of
dropping them. Prior failures, timeouts, and unrun checks move first with their
selected prerequisites.

Fast Python checks defer the inventory-classified gameplay-verifier group to
the medium lane. Deferrals bind source/index inputs and remain `DEFERRED`,
never reusable passes. Candidate and full checks retain complete Python
discovery under a separate receipt identity.

Receipts under `build/ai-validation/receipts/` are reused only when inventory,
source inputs, command, repository identity, CTest graph, tools, environment,
and native prerequisites match. CTest receipts also bind declared generated
executables. Changed source, setup, or build output invalidates affected
receipts; logs remain under `build/ai-validation/logs/`.

The full lane retains all configured release gates for an affected module; it
does not select a release level. Those gates still run through
`ai_check.py`'s candidate gate, which applies snapshot/minor/major eligibility.
CTest does not replace Mac, Windows, Android runtime, GPU, or multiplayer
acceptance. Claim budget compliance only after the measured complete lane,
including setup and builds, fits its limit.
