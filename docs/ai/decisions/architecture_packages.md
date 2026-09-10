# Package architecture through Snapshot 8

The owner's architecture/repository addenda extend this minor's roadmap.
These are implementation requirements, not completion evidence. S3 finishes
its existing foundation; extraction starts before S4 gameplay changes.
Standalone preparation may use frozen S3 inputs in isolation; game dependency
migration still waits for S3 publication and reviewed dependency revisions.

## Ownership and builds

| Repository | Separately built responsibilities |
|---|---|
| Private [CoreCpp](https://github.com/CoreJust/CoreCpp) | Core utilities; Runtime stateful generic services |
| Private [CoreProject2026](https://github.com/CoreJust/CoreProject2026) | Generic Script interpreter and DSL extension machinery |
| MinecraftClone | Shared game systems/DSL, Client, Server, game TestSupport/tests |

Core contains algorithms, containers, values and small wrappers, not rendering,
networking or executor subsystems. Runtime owns generic network, executors,
allocation, graphics, kernels and audio. Shared owns protocols, world storage,
generation and physics; Client owns scene rendering and visual LOD policy.

Expose component-aware installed CMake packages, not cross-repository source
includes. CoreCpp targets use `CoreCpp::`; Script uses
`CoreProject2026::Script`. A server-only build must not require Vulkan, GLFW,
audio or window initialization. Optional graphics/platform components carry
their dependencies. Each repository owns its README, minimal future-work ledger and tests.
TestSupport is opt-in and never a production dependency.

Pin dependencies to immutable commits in a lock file. Export the actual CoreCpp
revision and require Script's CoreCpp revision to match the game's selection.
Release provenance records all three revisions, toolchains and dirty states;
unknown or dirty dependencies cannot be claimed as exact releases. Private CI
access must be narrowly authorized; never publish personal tokens or keys.

## Runtime graphics, kernels and audio

Provide a usable Vulkan service boundary with encapsulated resources, device,
render targets, submission, presentation and readback. Separate ordinary
texture targets from presentation so no window is needed for subsystem tests.
Implement only operations needed by current consumers; do not build a full
multi-backend RHI. Future backends must not leak into Core or game simulation.

Kernel accepts SPIR-V files/bytes, validates/reflects entrypoints and bindings,
caches modules/programs and compiles GPU pipelines. Support graphics use and
standalone compute with an actual output-buffer test. High-level source
compilation and Script-to-kernel lowering remain future work. Resource lifetime
and cache keys include relevant device/program identities. Reuse warmed frame
storage; avoid per-draw allocation and frame-path `waitIdle`.

Audio needs bounded voice/control storage, deterministic offline mixing and a
native output adapter. Keep device drivers/decoding outside the real-time mix
callback. Prefer an established minimal backend over custom OS audio stacks.

## Generic Script and game DSL

CoreProject2023-2025 inform design; do not import unfinished compiler scope.
Use a bounded typed interpreter with explicit entrypoints, values, calls,
conditions and bounded iteration. Frozen source-compatible C++ extension
registries provide checked builtins/types; per-run host environments provide
capabilities without globals. A stable binary plugin ABI is not part of this
minor. Keep diagnostics, fuel, memory/output budgets and cancellation explicit.
No arbitrary OS access, native symbol lookup or future compiler/VM/JIT claim.

Shared defines game operations and stage schemas. Preserve `scenario 1` and
`flat2d-v1` as a fully supported compatibility frontend, not generic Script's
grammar. S4 onward documentation may prefer the new Core language. Both forms
use the same host operations for headless tests and visible scripted playback.
Provide complete navigable language and game-DSL references with tested examples.

## Generation, materialization and LOD

Implement optional pregen and registered XY refinement extents 16384, 4096,
1024, 256 and 64. Skip absent entrypoints; later stages read completed ancestor
and predecessor data. Ordered virtual-chunk stages publish bounded immutable
results atomically. Only the final current-revision stage materializes a chunk.

Validate script-declared distance/view/movement priorities and bound jobs,
queues, outputs, resident data and uploads. Stable seeded generation cannot
depend on traversal or worker order. Discard stale/cancelled results. A failed
stage permits only an explicit bounded retry, never automatic infinite retries.
Script configuration changes create a new generation identity. Rich geography,
structures, decoration and lazy loot remain future content tasks.

Preserve the owner's “from the client” direction: clients send intentions,
never speculative generated world data. Server-originated coarse visual
previews are a separate design choice. Interaction requests completion and
waits; never run an unbounded generation chain in a physics step. Unknown
collision space is not air. Revalidate queued actions after materialization.

Support mesh LOD variants and revisioned coarse/final chunk previews, with
hysteresis and atomic replacement. Physics never reads visual proxies. Test
seams, cancellation, eviction/regeneration, inherited state and bounded travel.
Behavior LOD remains an explicit future extension.

## Delivery sequence

S4 extracts packages, establishes generic Script and Runtime/Client texture
tests, then delivers camera/controls. S5 adds block/mesh and compute-kernel
contracts. S6 integrates staged sparse generation, audio and chunk previews.
S7 adds wrapping-aware LOD, resource/performance instrumentation and profiling.
S8 integrates permissions/physics with materialization and finishes subsystem
and platform acceptance. After S8 publication, record real evidence and report;
keep the minor active until the owner's feedback. Never invent S9 prematurely.

The implementation ledger keeps existing product tasks and adds prerequisites
without cycles:

- **S4 / `35`:** CoreCpp extraction `48`; Script/Shared compatibility `49`;
  Runtime Graphics `50`; graphics Kernel `51`; offscreen goldens `52`;
  Runtime/Shared networking `53`; product task `3`.
- **S5 / `36`:** standalone compute `54`; mesh-LOD foundation `55`; product
  task `4`.
- **S6 / `37`:** executors `56`; audio `57`; sparse-world product task `5`;
  staged generation `58`; revisioned previews `59`.
- **S7 / `38`:** wrapping product task `6`; visual LOD `60`; full-game
  performance work `61`.
- **S8 / `39`:** physics/permissions product task `7`; materialization `62`.

Tasks `63`-`65` retain rich geography, structures/decoration/lazy loot and
behavior LOD as future backlog. Task `42` establishes artifact workflow only;
each snapshot aggregate requires real GitHub assets, hashes, exact macOS/Android
runtime results and separately recorded user Windows runtime.
