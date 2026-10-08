# S7 delivery hindsight — 8 October 2026

## Finding and current state

S7 is still unreleased. The ten-day delay came from late qualification, inconsistent local setup, candidate drift, weak CI monitoring, and my failure to keep the parent task active through completion. Real cross-platform defects needed repair, but do not excuse avoidable retries, stale promotion, unsupported estimates, or repeated stops. Your feedback did not cause this delay.

At this checkpoint, `origin/ai-dev` `9a2a0b6` passed only Ubuntu/Windows fast checks in [37760659182](https://github.com/CoreJust/MinecraftClone/actions/runs/37760659182); release jobs were skipped. `ai-main` `a3d49d2` failed its macOS analyzer row because `scan-build --version` is unsupported. The `--help` fix is in newer source, but `a3d49d2` has older source parent `daaf4a3`, not `9a2a0b6`. Its artifact workflow [37738321056](https://github.com/CoreJust/MinecraftClone/actions/runs/37738321056) failed waiting for the matrix. I cancelled the already-failing analysis run after Android HWASan stayed queued for hours. There is no release tag, final artifact set, published-byte runtime evidence, or accepted screenshot set.

## Why delivery took so long

The remote history has 41 `MC-AI-0038` commits on `ai-dev` and 15 first-parent promotions on `ai-main`. This is candidate churn, not active time. My repeated six-hour estimates and two-day counterfactual had no measured basis; I should not have presented them as reliable.

Local setup failures repeatedly preceded expensive checks. The fresh 8 October worktree lacked `build/debug`; `VCPKG_ROOT`, `VCPKG_INSTALLED_DIR`, `MC_PRIVATE_DEPENDENCIES_PREFIX`, `VULKAN_SDK`, and `MC_CMAKE_PREFIX_PATH` were missing or inconsistently carried. A reused cache held private packages that mismatched source locks. After pinned configuration, the full local gate passed: the initial failure was setup, not source. Another hook omitted `VCPKG_ROOT`, skipping a pinned-tool test until rerun.

I confused execution context with Mac/game failure. Restricted execution denied `/bin/ps` and renderer startup hit XPC/Launch Services; the identical gate passed with host permission. Earlier launcher output showed invalid arguments. My Launch Services repair produced no app records; I should have checked invocation and permission first.

Some failures were real: sanitizer/tool incompatibilities; an unsupported macOS analyzer probe; a 201.5405 ms service interval against a 200 ms test bound; Windows path, CRLF, and host-dependent test assumptions; Android acceleration mismatch; and movement, collision, camera, and launch issues found in playtests. Tests did not protect these boundaries early enough. Their fixes are in the `MC-AI-038x` records, but local results do not establish hosted acceptance.

Release coordination multiplied rework. Candidates changed repeatedly; a promotion parent did not match the newer source I believed I was advancing. An Astra review was unusable under FF/receipt rules, and I launched excess reviewers rather than sending only the correction to the same reviewer. A metadata change on the immutable promotion side was correctly rejected and redone. The official finish gate and normal commit hook each run a full candidate check; both are required today, but duplicative. I waited on Android after the Mac row failed at 07:17 UTC; that runtime stayed queued until cancellation at 10:45 UTC. I also stopped or handed off after intermediate outcomes. A completed subtask is not S7 completion.

## Improvements

1. Preflight exact checkout/SHA/tree/merge parent, task trailer, staged scope, build preset, dependency locks and package identities, tool versions, required environment, host permission, and renderer/runtime capability before a full gate.
2. Freeze one candidate per correction batch. Finalize code, acceptance wording, task records, screenshots, and evidence on `ai-dev` before review. Assert mechanically that promotion uses exactly the reviewed source head and expected tree.
3. Monitor every required CI row by workflow ID and exact SHA. Show passed, failed, queued, and skipped separately; surface terminal failures immediately; preserve receipts/logs; cancel obsolete jobs after recording the failure. An aggregate wait cannot hide a failed row.
4. Test production boundaries early: one-server/one-client launch, visible Mac renderer, Windows fixtures, Android lifecycle/acceleration, and accepted movement/camera/network scenarios. Re-run the named host checks after source corrections.
5. Keep every candidate, hook, matrix, strict/tag, artifact-integrity, and runtime gate. Reduce structural duplication only through checker-verified receipt reuse for identical inputs with stale-input negative tests—never by bypassing hooks.
6. Keep the parent task active until publication, downloaded-byte integrity, Mac/Android runtime, screenshots, Windows user acceptance, and the final ledger are resolved. Progress and estimates must reflect measured evidence and the remaining critical path.

Fail-fast CI inspection, retained diagnostics, platform test fixes, and the supported analyzer probe are already on the source line but remain unqualified for this candidate. This review does not declare setup repaired before release evidence passes.

## Evidence limits

Evidence: Git parent/tree data, task ledger, CI runs/row receipts, and recorded local gates. See [source fast run](https://github.com/CoreJust/MinecraftClone/actions/runs/37760659182), [promotion matrix](https://github.com/CoreJust/MinecraftClone/actions/runs/37738321164), [artifact workflow](https://github.com/CoreJust/MinecraftClone/actions/runs/37738321056), [release policy](RELEASES.md), and [MC-AI-0038](tasks/MC-AI-0038.md). Commit counts are not active-time measures. I have not reconstructed every reported stop from the full transcript and will not invent timestamps or attribute them to a single system fault.
