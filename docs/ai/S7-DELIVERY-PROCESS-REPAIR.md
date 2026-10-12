# S7 delivery process repair

S7 remains unreleased. MC-AI-0434 repairs its workflow;
this document does not claim qualification is complete.

## What was structurally wrong

- `ai_publish.py prepare`, `finish`, the aggregate commit hook and `tag` could
  repeat full candidate/strict checks. `ai_check.py` disabled candidate/strict
  receipt reuse and explicitly excluded build/CTest from reuse.
- Analysis receipts required the entire candidate commit/index identity.
  Documentation and task-ledger changes therefore invalidated expensive native
  checks even when their actual inputs were unchanged.
- Artifact packaging waited for a hosted matrix for up to 21,000 seconds.
  Qualification, promotion and packaging formed a serial retry chain instead
  of independent, bounded steps.
- Environment defects were discovered during late qualification attempts:
  fixed setup-tool paths, Java configuration, dependency/toolchain mismatch,
  emulator mapping and sanitizer runtime compatibility. Successful builds were
  repeatedly mistaken for readiness to finish the release.
- Phase logs were buffered until subprocess completion. A completed phase or
  a heartbeat did not establish the current candidate's qualification status.

## Replacement contract

One coordinator owns a finite local qualification plan. Preflight checks tools,
dependencies, runtime capability and output consumers before expensive work.
Each required check has a relevant-input identity and an actual result; reuse
requires validated source, dependency, configuration, tool and command matches.
Missing, stale, failed or unavailable evidence stays distinguishable and blocks
the applicable required gate. Metadata-only changes do not restart native work.

Release authenticity separately verifies exact source/promotion parents/tree,
trailers, immutable tags and artifact hashes. GitHub builds and packages only;
it does not wait for qualification, run analyzer/emulator jobs or certify runtime
acceptance. Checks and wrappers share one gate owner rather than independently
rerunning the same expensive suite.

Windows and macOS remain primary. MinecraftClone Android temporarily requires
local tests, basic runtime evidence and the APK. This reduction does not apply
to CoreLang: its Android support remains equal to Windows/macOS.

## Proof required for this repair

Regression tests must demonstrate metadata reuse, relevant-input invalidation,
failure/missing-proof rejection, exact release authenticity and artifact-only
CI. Real local qualification and platform runtime evidence remain separate
delivery gates. Reports must cite observed results, not estimates or rebound
historical passes.

## Local runtime setup findings

MC-AI-0435 corrects two reproduced MSan setup defects. Instrumented libunwind
failed the exception control on the local ARM64 analysis host. LLVM's own
sanitized-library recipe instead builds instrumented libc++/libc++abi against
the system unwinder; the unchanged exception and heap-read controls passed with
that configuration. This is not evidence that every earlier unwind failure had
the same cause.

The negative control then stalled because LLVM's symbolizer inherited Ubuntu's
remote debuginfod URL. A traced HTTPS lookup and a bounded offline control
identified the cause. Clearing `DEBUGINFOD_URLS` only for the verification
process preserved symbolized source/allocation diagnostics and completed both
controls. Machine-wide settings and sanitizer acceptance predicates were not
changed. The full game MSan row remains a separate qualification requirement.

Windows preflight also found that the first local receipt implementation omitted
the selected Vulkan SDK resources and MSVC ASan DLL. The corrected identity
captures cache-selected headers, loader and shader tools, plus the imported
ASan DLL. It does not assume all resources come from one SDK: the actual Windows
and Mac builds use vcpkg headers alongside separately installed tools/loaders.
The Windows compiler probe failure was a compound-CMD `%PATH%` expansion error;
the toolchain initialized correctly with separate command lines. No machine
repair was needed for that failure.

## Native-service and receipt-boundary findings

The same Mac exposed different Launch Services state inside and outside the
execution sandbox. The sandbox view contained zero registered bundles; the
native view contained 635 and an initialized system cache. The identical ASan
AudioUnit control could not obtain the output component inside the sandbox,
but outside it observed a real callback and successful stop, uninitialize and
dispose. Its intentional application leak remained detectable. This evidence
does not justify resetting the healthy system cache or disabling sandboxing
globally: native-service acceptance needs the scoped, reviewed execution
boundary. It also does not prove the cause of every historical launch failure.

The first full native ASan run passed its test bodies but failed six processes
on exit-time Apple audio-framework leak reports. Runtime teardown is present;
an isolated pinned-CoreCpp runtime-output reproduction then reported the same
376 bytes in eight Apple-framework allocations after successful start/play/stop.
Adding an owned 37-byte allocation produced a separate, symbolized application
leak report. Transport tests can use their existing audio injection boundary
without changing production audio or suppressions; native callback controls and
the reproduced framework limitation remain explicit. The original failed rows
are retained, not rewritten as successful qualification.

Real receipts exposed gaps that synthetic fixtures had missed: the Mac verifier
resolved a recorded Linux prefix through its own filesystem, and cppcheck's
capture loop overwrote the required configuration key. Analyzer retries also
encountered stale invocation records from an earlier compiler-argument set.
Capture-to-validator round trips and foreign-path fixtures must cover these
boundaries; a successful analyzer command alone is not a valid receipt.

The Android launcher rejected the instructed shared default emulator because
it still required a historical S7-specific AVD. Basic runtime evidence was
obtained on the default device with exact serial, API, ABI and installed-APK
checks, but that evidence does not establish correctness of the old launcher.
The repaired guard retains duplicate-client protection.

Fresh CMake configuration preserves object files. IWYU/cppcheck must clean
generated objects before building or their launchers skip current translation
units; clean-first presets automate this without deleting raw reports. Live
CTest output uses randomly suffixed `LastTest.log.tmp*` files, not one fixed
name. Canonical-tag-only artifact triggers avoid duplicate promotion builds.
