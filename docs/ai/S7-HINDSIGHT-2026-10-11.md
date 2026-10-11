# S7 delivery hindsight — 11 October follow-up

This supplements the [9 October checkpoint](S7-HINDSIGHT-2026-10-08.md). S7 is still unreleased; passing implementation checks is not publication.

## Additional proven failures

- **A green job concealed an unavailable Windows gate.** Promotion `2ec4aba`, run `38096415002`, reported Windows Debug success, but its artifact contained only `windows_msvc_asan.json`, marked unavailable. `cl.exe /Bv` printed a version and exited 2 because no source was supplied. Native MSVC 19.44.35229 reproduced this; `/Bv /?` returned 0 with the same version. The CMD loop expanded `%errorlevel%` before executing its failing command: a native inner exit 7 became exit 0. Explicit exit 1 propagated failure. MC-AI-0421 (`e2bae0b`) fixes both roots, preserves actual analysis commands and unavailable handling, and adds regressions. Its 74 focused tests and 579-test development gate passed, with one intentional development-suite skip. Corrected-source hosted analysis is still required.
- **The coordinator missed a cheap date preflight.** A full candidate passed 576 tooling tests, 595 CTest cases and renderer smoke, then failed its snapshot date check. Native CTest alone took 1168 seconds. Correcting the heading required another full candidate. Preflight immutable source/date and publisher metadata before long gates; do not claim a failed aggregate invocation passed.
- **Host contention amplified repeated qualification.** Two tooling runs exhausted their 300-second budget while 37 compiler/analyzer processes and about 20 GB swap were observed. The stopped fixture passed alone in 8.664 seconds; after contention subsided, the full tooling suite passed. This supports contention as a contributor, not an explanation for every historical delay. Only verified task-owned process groups were stopped; unrelated work was preserved.
- **Continuation notes overstated unused authority.** Live readback showed the earlier ephemeral attempt had already run as `38071858357`. No new registration was made from that stale claim. The user explicitly granted one attempt and retargeted it to the corrected successor. Verify/report its exact SHA and run before requesting a token; record consumed attempts separately from planned ones.
- **Required static-analysis closure was incomplete.** The same promotion passed macOS ASan, UBSan, TSan and Clang static analysis, then stopped at clang-tidy. IWYU and cppcheck never ran in that job. A complete local LLVM 23.1.3 diagnostic scan subsequently flagged 71 of 125 translation units, including repeated header findings; this is not a count of independent defects. MC-AI-0422 batches source repairs and verifies analyzer context before the next promotion. The earlier “implementation complete” label concealed this unresolved release work. Run every required analyzer as an early diagnostic batch, then freeze and qualify the exact candidate; stopping discovery at the first failed translation unit creates serial promotion churn.
- **The corrected Windows probe exposed a dependency-configuration defect.** Promotion `2ff93b78`, run `38138670741`, passed the normal Windows Debug build and 596 registered native tests, with one documented skip. Its required ASan row then failed linking: the Release sanitizer preset reused Debug private dependencies, producing runtime-library, iterator-level and sanitizer-annotation mismatches. The earlier unavailable-tool defect had hidden this next failure. MC-AI-0425 must qualify the entire instrumented dependency closure and both complete native Windows analysis rows before another promotion. Ordinary Debug/Release tests do not establish sanitizer compatibility.
- **A manually composed promotion message wasted another full gate.** The unpublished merge used the wrong canonical subject. Although its source checks passed, bootstrap validation rejected it. The coordinator preserved the original commit, obtained approval for a message-only amendment, verified unchanged tree and parents, and pushed corrected commit `2ff93b78` through the normal hooks. Use the publisher's canonical promotion contract and fail-fast command sequencing; passing an expensive gate does not excuse a cheap metadata preflight.

The [native-analysis follow-up](S7-HINDSIGHT-2026-10-11-ANALYSIS.md) records
the subsequent setup, tool-version, compiler and runtime boundaries.

## Process changes and remaining work

Analyzer setup also confused advice with defects. CMake's cppcheck hook dropped
the compiler's standard, system-include and implicit platform context; an
information-only report still returned the configured failure code. IWYU asked
for Apple and GLM implementation headers, alongside genuine missing public
includes. MC-AI-0422 replaces that integration with complete per-file reports,
explicit blocking categories and real compiler context. It preserves required
analyzers and defect checks without forcing cosmetic changes or private includes.
An actual CMake adapter probe also showed that wrapper exit 7 became success
without an outer `-Xiwyu --error` option. Keeping that adapter option, but
stripping it from the IWYU subprocess, propagates defect failures without
making removal advice fatal; both paths now have a regression test.

A delegated include pass also selected the primary checkout instead of its
assigned delivery worktree, then incorrectly reported 13 files absent. The
coordinator checked absolute paths, required removal of only that worker's
include additions, and verified the corrected worktree pass. The primary's
unrelated tracked and untracked work remains preserved. An assigned path is not
proof of execution there: verify the actual working directory before edits and
validate changed-path scope when accepting a worker receipt.

The documented direct no-commit merge path removed the prepare/finish wrapper's duplicate checks, while preserving review and mandatory commit gates. Receipts are inspected as data, not inferred from job colors. Native tool/shell preflight now catches the Windows failure before another promotion.

Future checker work should investigate fail-fast execution and exact-input receipt reuse; neither is silently introduced to bypass this release's gates. Keep one corrected batch frozen, complete the required matrix and runtime/artifact acceptance, then tag and publish. No minor publication or Snapshot 8 is authorized.
