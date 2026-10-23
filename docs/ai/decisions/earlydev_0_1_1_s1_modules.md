# S1 — Project review and module separation

Read-only source review at `7b33f956296045520cdf6525bafb62f594f3386c`
identified the following concrete separation work. This records the review
and its concerns now; implementation must verify the resulting dependency
graph and custom modes rather than claiming existing folders are modules.

## Evidence and concerns

| Current source | Concern and required change |
|---|---|
| `src/CMakeLists.txt:1–64`; `src/shared/CMakeLists.txt` | One `mc` target combines shared world, CoreLang, protocol/scenarios, server and optional client. Server-only builds remove graphics but do not expose the reusable numerical target. |
| `src/server/GameServer.cpp:337,754–810,861–880` | Generation workers and dispatch are embedded in the server; default/thread-local generators can bypass the selected world context and inherited refinement. Extract the service, then inject it into every consumer. |
| `src/shared/world/SparseWorld.cpp:98–208` | A preview is not authoritative block data, and synchronous queries can instantiate another default generator. Make readiness explicit and share the canonical context. |
| `src/client/GameClient.cpp:75–107`; `src/client/PlayerClient.cpp:1138–1152` | Join proceeds automatically and continuous look starts at construction. Introduce the loading/key gate before normal input activation. |
| `WorldGeneration.cpp`, `CoreLangScenario.cpp`, `CoreLangPolicy.cpp` | Compiler/runtime/host contracts have separate owners; policy JIT does not prove generation JIT or AOT. Use a shared backend/provenance adapter with focused consumers. |
| `tests/client/game_client_benchmark_tests.cpp:38–68` | A stub reports presentation success. Keep it as a control-flow fixture; actual frame-completion and GPU timestamps own FPS acceptance. |

Paths are relative to `src/shared/world`, `src/shared/scenario` and
`src/shared/policy` for the three unqualified CoreLang filenames above.
Source line anchors describe the reviewed baseline, not future file locations.

## Target responsibilities

Split by actual consumers, without creating a generic plugin framework:

| Boundary | Responsibility and consumers |
|---|---|
| CPU world/generation (`mc_generation`) | Coordinates, materials, validated configuration, atlas kernels, canonical regional/surface/chunk sampling and raw map encoding. Shared by game, exporter and reference tests; no window, Vulkan, network or compiler initialization. |
| Generation service | Scheduler, cache/dependency leases, workers, readiness and publication. Extract server-embedded workers; normal game and 2D replay use this same service. |
| CoreLang adapter | Exact dependency/backend lifecycle, bounded plan/host declarations and executable artifacts. Used by generation planning, scenarios and policies; keep it out of numerical kernel dependencies. |
| Authoritative gameplay/transport | Simulation, collision, permissions, join/replication and streaming. Headless integration and two-laptop games use the same authority; protocol/codec tests require no renderer. |
| Application/presentation | Loading/input state machine and immutable scene/map/coverage views. Select Vulkan or in-memory column-color adapter; neither owns a generator. |
| Test support | Shared replay/clock, exact-key/coverage, pixel/field and timing probes. Reuse production constructors; adapters replace presentation or observations, not generation. |

Some boundaries may initially be internal libraries/components rather than
one target each. Enforce the important dependency directions with standalone
consumer builds: numerical exporter without graphics or server/network startup;
headless generation service; normal-game 2D composition without Vulkan; ordinary
Vulkan composition; CoreLang backend consumers. Keep Android packaging and
desktop entrypoints as adapters to these same services.

## Completion evidence and further review

Audit ownership, cancellation, thread affinity, cache/queued/leased bytes,
stale epochs, packet validation and failure propagation across the boundaries.
Remove silent default-generator fallbacks from production paths; keep explicit
fixtures isolated. Uniform chunk representations must preserve mutable edit
and collision semantics. A renderer cannot advance logical readiness merely
because it drew a preview.

Follow the [testing matrix](earlydev_0_1_1_s1_testing.md) for correctness and
performance per subsystem. Add a CMake dependency/consumer check and exact
backend/source identity to receipts. Review the implemented split and residual
concerns before S1 acceptance, using source, headless/custom tests and actual
game evidence. The read-only audit is not proof that those changes exist.
The 320-block sphere includes volume, not merely roughly 1,300 XY columns.
`Chunk.hpp` stores 4,096 one-byte blocks per dense chunk; enumerate exact Ready
keys and account for chunk payload, metadata, meshes, leases and prefetch
before choosing the native memory budget. The prototype atlas RSS is not this
working set.
