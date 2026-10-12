# S7 delivery process repair

The owner changed the release workflow after repeated failed delivery attempts.
S7 remains unreleased. MC-AI-0434 repairs the process; this document does not
claim that its implementation or qualification is already complete.

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

These causes are supported by the gate call graph, receipt validators and
retained task evidence. They do not justify the delay, and removing a hosted
gate alone does not fix the underlying identity or repeated-execution defects.

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
delivery gates. Review and the final delivery report must cite observed results,
not estimates or historical passes silently rebound to a new candidate.

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
