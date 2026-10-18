# 16 October correction: tool sandbox, not a required OS repair

Inside the sandbox, the unchanged CC-0044 Vulkan binary passed 43 tests and
skipped 15 GPU tests because Metal was unavailable. The same binary passed all
58 tests unsandboxed in 278 ms. XML results remain in the CC-0044 checkout:
`build/cc0044/vulkan-tests.xml` and `vulkan-tests-unsandboxed.xml`.

The unchanged game reproduced Launch Services errors and its 10-second capture
watchdog in the sandbox. The identical `--capture-render` command exited 0 in
under a second in approved host context, producing image/pass evidence. Reports
at `/private/tmp/mc-s7-capture-context-c.HYT9DN`: sandbox JSON SHA-256
`997a23467d19d39c7d51d5b44e059a696c0b9c2659519e52b7471fb9fc1c0b79`; host JSON
SHA-256 `26c10a477f2a440a88c0ed9542cfa273560ca0bef68086af1174f79ff329a6a3`.
The sandbox failure hash matches the earlier retained startup failure.

This isolates the current failure to execution context, not every historical
host diagnostic. Earlier investigation escalated to OS repair without
controlling context; this comparison changed no OS setting, service, cache,
registration, or reboot. Further OS repair remains prohibited. Record approved
host context for Mac GPU/window acceptance. Capture does not qualify the new
dependency revision, terrain continuity, cadence, or release; exact-source gates
remain separate.

Another delegated package launch omitted context and blocked in
`_glfwInitCocoa`/`NSApplication run` before MoltenVK. The same package/arguments
started in approved host context. Executor packets must specify launch context;
preserve failed-launch cleanup and do not misclassify sandbox Cocoa failure as
an OS defect.

## Test the shipped Mac runtime, not the global development runtime

The raw benchmark used `/usr/local/lib` Vulkan: read-only process maps showed
the global loader/MoltenVK, while the release launcher selected newer bundled
SDK libraries. Build/source hashes alone missed this difference.

Through the normal verified package in host context, the unchanged executable
passed existing offline Mac cadence predicates in all three R128 phases, with
no inferred lost FIFO slots. This is not physical-display or release
qualification. The recorder requires/retains that package, checks launcher,
ICD and library closure before/after, and rejects unbound direct-Mac captures.
Its Python suite passed 98 tests; exact-source captures remain required.

Profiling showed most sampled CPU work inside `complete_present_wait` was
synchronous MoltenVK draw encoding during `vkQueueSubmit`, disproving a pure
FIFO-wait interpretation. Keep this combined metric distinct from active
compute, cadence and physical-display timing; its name is not causal evidence.

## Make archive overlays invalidate changed build inputs

The deterministic source archive stores zero modification times. An incremental
Windows overlay therefore initially ran only two shader-copy actions despite
changed C++ bytes. Before any runtime test, comparing the old/new file hashes
and advancing only changed input timestamps forced the real 59-step rebuild.
The task-local transfer helper now performs that invalidation automatically
and records paths, old/new hashes and timestamps. A successful no-op build is
not evidence that newly transferred source was compiled.

## Sanitizer and analyzer recovery

Apple Clang 17's AddressSanitizer stalls during shadow initialization on this
host; its ThreadSanitizer crashes before `main`. Both failures also reproduce
outside the tool sandbox, so host execution alone does not repair them.

The task-local official LLVM 23.1.3 compiler/runtime resolves these startup
failures. Its bundled libc++ headers introduce a separate ABI mismatch with
the host runtime: a nontrivial `unordered_map<string, int>` program fails to
link with an undefined `std::__1::__hash_memory`. Selecting the verified
macOS 26.2 SDK's libc++ headers and sysroot fixes that mismatch without changing
the host runtime. Clean ASan and TSan probes then exit successfully; deliberate
heap overflow and data-race probes produce the expected sanitizer reports.
An empty-program success alone would have missed this second defect.

Recovered LLVM, IWYU 0.27 and Cppcheck 2.22.0 live under
`/Users/core-just/.codex/s7-toolchain-recovery-c`. The manifest records versions,
source identities, hashes, licenses and smoke results. Its
`run-macos-analysis.sh` wrapper applies only process-local paths and SDK flags.
Fresh CMake caches must select the recovered compiler; existing Apple Clang
caches do not become qualified merely because `PATH` changes. Game rows still
need their actual configured checks: tool smoke tests are not game passes.

## Do not make remote publication a local-diagnostic prerequisite

The owner's dependency rule requires tagged `ai-main` CoreCpp commits. I
initially treated remote publication as a prerequisite even for local engine
diagnostics, coupling game work to the shared publishing-guard repair. That
additional ordering was not required by the stated rule. After full local
producer qualification and exact review, normal local fast-forward promotion
and an immutable local tag provide the required development dependency.

Keep the boundaries explicit: this does not fabricate `origin` ancestry,
bypass acquisition checks or authorize remote writes. Final release still
requires actual remote branch/tag publication and independent readback. The
correction lets local maintenance-package and engine work continue while that
separate publication prerequisite is repaired.

## Select the actual Windows runtime; validate foreign paths by their platform

The installed Vulkan SDK correctly lacked `Bin/vulkan-1.dll`: LunarG documents
the loader as a GPU-driver component. Our CMake requirement invented an SDK
layout and forced a temporary runtime-directory view. The owning build rule now
accepts an explicit absolute `MC_VULKAN_RUNTIME_DLL`, verifies the file, and
stages that selection. Analyzer provenance reads the same cache value. The
real SDK and System32 loader configured and built successfully without changing
the SDK or system installation.

The Windows focused tests then exposed an offline-verifier bug: it interpreted
recorded macOS/Linux paths using the Windows verifier's native `Path` flavor.
Using the already existing recorded-platform pure-path helper fixes that
boundary; a regression simulates Windows semantics across every manifest row.
The corrected Windows selection passed16/16 in3.845s. This is setup evidence,
not a passing terrain or cadence result; the subsequent production capture
still reproduced the in-motion mesh gap.
