# S1 — CoreLang 0.1.5 dependency contract

The owner requires CoreLang0.1.5 JIT/AOT. The latest scope amendment explicitly
allows an exact ai-dev revision for the S1 snapshot; final CoreLang release is
not an S1 prerequisite. Preserve the strict current23f81441/CoreCpp5c1bad pair
below unless a reproduced integration defect requires an update. S1 still needs
passing tests, required performance and owner playtest. The other-chat terrain
model replacement is deferred to S2; existing R6/simpleLOD/curvature stay in S1.

## Temporary pin, fixes and release

The previous `ab1a8f7d2e4ff1c06e7640a3707d1ba63e89df7a` candidate is retained
as historical evidence: its lock required CoreCpp
`1a1d4095bd553ed1ea382523eecd22c9748bbe9f`, while its task-private diagnostic
build used CoreCpp `63004f816348778e95703e2d92061531a4bcb277` with the inexact
override. That package was incompatible with the game pin and is not used here.
The older `9840659b27dbd6feaf99fa932dde19107e51d720` release pin and owner
feedback packages remain preserved separately for rollback.

For this superseded pin, Release packages were built/installed on Mac
(AppleClang 17) and Android arm64-v8a/API 28 (NDK 27.0.12077973, Clang 18.0.1),
using exact CoreCpp `63004f8` with provenance OFF. Metadata matched clean exact
pins; Android CLI was ELF AArch64. Logs, caches, metadata, and hashes:
`/Users/core-just/.codex/task-evidence/mc-s1-20261019/corelang-exact-cb/`.
These builds disabled CoreLang tests and prove neither game/backend behavior,
parity, nor runtime acceptance. Snapshot use requires immutable revision/locator and dependency, parity and
platform checks; a final language release is unnecessary. Never bypass provenance for a
working-tree library.

Report reproducible integration defects to CoreLang and upgrade only to an
owner-supplied exact commit after focused regression, fixture/backend parity,
and affected-performance checks. Defer new features; keep the game lock,
package provenance, and receipts aligned, preserving prior pins for rollback.

## Backend and module obligations

The bounded executor and adapters now expose explicit Interpreter, JIT, and
AOT selection for terrain-generation plans, scenarios, and policies. Each
successful execution retains a receipt with requested and actual backend,
source/bytecode digests, compiler version, and target; AOT receipts also carry
the native-image digest. JIT is required with full coverage, and AOT uses the
native image path. Either request fails when unavailable; neither silently
falls back to the interpreter. Scenario and policy consumers compare selected
non-interpreter output with the interpreter by default. Generation returns
the selected backend receipt with its bounded manifest. Historical inexact adapter tests do not qualify strict game behavior.

Static execution checks allow finite `for` loops with matching, nonnegative
fixed-width integer literal bounds and a named immutable index. Writes,
aliases, captures, or address escapes of that index are rejected, as are
dynamic loops, imports, indirect calls, and recursion. Source proof bounds
compiled loop cycles and a checked reachable-call-graph walk bounds direct
call costs; unproven bytecode cycles fail. Unsupported static-init bytecode
also fails. These are preflight bounds, not VM fuel. Cancellation is checked
during source analysis/compilation, between backend phases, and at host-call
boundaries, but cannot interrupt an individual runtime or native call. Strict game checks must verify the integrated package.

Keep deterministic CPU terrain kernels shared and CoreLang compilation/host
binding in an adapter. JIT/AOT execute the bounded plan and host calls without
duplicating terrain. Include world-specific compile/load cost in loading;
assess process/runtime/thread-local ownership and retained executable memory.
Record backend/compiler options and provenance. Optimization must preserve
Terrain-v2 content for a fixed version, seed and configuration.

Checked-in `scenarios/world/canonical_world.core` and `height_tile.core` use the
unquoted `@version(0.1.5)` accepted by `parseScenarioSource`; older quoted
0.1.2/0.1.3 examples are rejected and are not qualified.

Minecraft runtime and S1 product gates remain open. The historical `ab1a8f7` mismatch and inexact override are recorded above; never
mix dependency revisions, modify compiler source, or bypass provenance.

## Exact compiled-fmt handoff (2026-10-21)

This pin supersedes the `cb6177f` / CoreCpp `63004f8` pair above for the next
strict S1 integration build. The earlier sections and receipts remain
historical evidence; the lock now pairs CoreCpp
`5c1bad11346cfad44470f5985f1944929952b9a6` from
`refs/tags/v0.1.0-cc-0050-compiled-fmt` with CoreProject2026
`23f81441c10a44896b87865a9b8ae7d4b51687db` from `refs/heads/ai-dev`.
The CoreProject `ai-dev` head advanced after the handoff, but the pinned commit
remains its ancestor and was fetched from the verified repository origin.

The Windows Release consumer used both exact packages and linked `mc_main`
successfully. Its binary SHA-256 is
`1382a7a9de71a2db95aa48aee529e6baa377f82b54544ce5877c7da1dd916cbc`;
Windows runtime remains unrun. The full Mac CoreCpp package reports clean exact
revision `5c1bad...`. CoreProject2026 was fetched into a clean detached clone
and built/installed with AppleClang 17, Release, and
`COREPROJECT2026_ALLOW_INEXACT_CORECPP=OFF`. Its installed metadata reports
clean exact revision `23f814...` and required CoreCpp `5c1bad...`. The full
CoreProject test build succeeded; CTest passed 94/95 tests. The sole failure
is `CoreProject2026.RepositoryLimits`, because the pinned source's
`docs/tasks/backlog.json` contains 1001 lines against a 1000-line limit. All
CoreLang, CoreRT, CLI, JIT/AOT, and package-consumer tests passed.

The private Mac package build, install prefix, CMake cache, logs, and hashes are
under
`/Users/core-just/.codex/task-evidence/mc-s1-20261019/corelang-exact-23f81441/`.
The first exact Mac CoreCpp prefix contained only Core and Runtime; strict
consumer configuration rejected it for missing `RuntimeExecutors`. It was
preserved and not used by Minecraft. A fresh clean-source Mac package was
built with the full client component set into `mac-corecpp-prefix`.

The shared strict Mac Minecraft build now uses the new exact pair with both
inexact overrides OFF. CMake configuration passed, and the `mc_main` plus
`mc_tests` build completed all 182 Ninja steps. The focused CoreLang and world
preparation filter passed 68/68 tests across five suites, including
`WorldPreparationTest.StrictBackendsPreserveCanonicalWorldAndOriginalRequestTime`.
A separate private instrumented probe passed 3/3 scenario/policy adapter
tests and recorded seven actual Interpreter, JIT, and AOT receipts; JIT/AOT
requested and actual backends matched, and both AOT receipts had nonzero image
hashes. The strict consumer contract and test receipts are retained under
`/Users/core-just/.codex/task-evidence/mc-s1-20261019/`.

These producer, link, and focused behavior checks do not establish Mac runtime,
Windows runtime or full S1 acceptance. Final CoreLang release qualification is
separate and does not block this snapshot.
