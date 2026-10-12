# Automation and extension points

Deterministic Python tools run locally without model calls or keys. Use Python 3.12+ (`python` on Windows when `python3` is unavailable).

| Tool | Purpose |
|---|---|
| `script/ai_plan.py` | Task/version lookup; [skills](SKILLS.md) |
| `script/ai_commit.py` | Review/commit validation; [protocol](COMMITS.md) |
| `script/ai_history.py` | Trace history and aggregate children |
| `script/ai_run.py <route>` | Local model runner |
| `script/ai_setup.py` | Prerequisite and hook setup |
| `script/ai_analysis_matrix.py` | Analysis receipts; see below |
| `script/ai_docs.py check` | Coverage and freshness |
| `script/ai_docs.py refresh` | Reviewed-doc refresh |
| `script/ai_tasks.py` | Task editing/rendering; see `--help` |
| `script/ai_check.py --fast` | Fast checks |
| `script/ai_check.py` | Build/test checks |
| `script/ai_check.py --strict` | Strict checks |

The local qualification matrix, revised by MC-AI-0434, is declared in
[`script/ai_analysis_matrix.json`](../../script/ai_analysis_matrix.json). Run a
host lane, merge passing local receipts, then verify offline. Repeatable `--row`
selects individual rows when their toolchain or dependency environments differ:

```sh
python3 script/ai_analysis_matrix.py run-local --root . \
  --receipt build/ai-checks/analysis-local.json
python3 script/ai_analysis_matrix.py merge --root . \
  --receipt build/ai-checks/analysis-aggregate.json --row-receipt <host-receipt> ...
python3 script/ci/verify_analysis_gate.py --root . \
  --receipt build/ai-checks/analysis-aggregate.json
```

Use a distinct receipt path per row and merge all required rows. Linux must set
`MC_ANALYSIS_LINUX_LSAN_TRIPLET` and `MC_ANALYSIS_LINUX_MSAN_TRIPLET` explicitly
to the matching x64 or arm64 analysis triplet; an absent selector fails before
configuration. Changing architecture requires matching instrumented dependencies
and libc++ runtime controls, not merely overriding the preset name.

Receipts bind relevant source/dependency/configuration inputs, tools, commands and diagnostics; missing,
unavailable or stale rows fail closed. macOS runs ASan with integrated LSan,
UBSan, and TSan; Windows runs MSVC ASan/analysis;
Linux uses tests-only LSan/MSan with libc++ and instrumented dependencies.
CTest rejects empty discovery, UBSan halts on diagnostics, and performance uses
ordinary Release. Release commit/tag authenticity is checked independently.
Android requires local tests, basic runtime evidence and its ordinary APK;
hosted HWASan qualification is suspended for this game, not for CoreLang.

Windows probes use `cl.exe /Bv /?`; `/Bv` alone exits nonzero. Failed probes remain
unavailable. CMD row loops use explicit failure exits, avoiding stale
`%errorlevel%` expansion. Green jobs never replace passing exact-candidate receipts.

IWYU/cppcheck wrappers require complete `src/`/`tests/` candidate-bound reports;
compiler, parser, configuration and coverage failures block. IWYU blocks missing
public includes; removals, forward declarations and unsupported private headers
remain advice. CMake's outer `-Xiwyu --error` propagates wrapper failures; the
wrapper removes it before running IWYU. Cppcheck retains compiler context. Owned
errors/warnings/performance/portability block, except reviewed design hints
`uninitMemberVarNoCtor`/`passedByValue`; uninitialized reads remain fatal.
Style/information/external-primary findings remain reported. Sanitizer gates are unchanged.

The macOS ASan row keeps `detect_leaks=1` and runs clean, ordinary-leak, and real
AudioUnit callback controls before configuring/building the game. Leak reports
come from LSan integrated with ASan; Clang documents macOS leak detection via
`ASAN_OPTIONS=detect_leaks=1` and LSan integration in its
[AddressSanitizer guide](https://clang.llvm.org/docs/AddressSanitizer.html#memory-leak-detection).
Do not wrap the ASan-instrumented process with Apple `leaks`: on exact promotion
5fd704e, that external inspector could not inspect the ASan malloc zone
(job 113144469210; see [MC-AI-0384](tasks/MC-AI-0384.md)). Callback evidence
requires observed, successful execution; zero OSStatus for setup, start, stop,
uninitialize, and dispose; and one 4096-byte app-leak block with
both `createApplicationLeak` and `renderAndLeak`. Ordinary leaks must retain
their 4096-byte app frame. The three exact LSan sites are
`AMCP::Utility::Dispatch_Queue::install_mig_server`,
`AutoreleasePoolPage::autoreleaseNoPage`, and `__CFTSDGetTable`. Row receipts
retain suppression counts and unsuppressed reports. These exact stack-frame
rules can exclude allocations regardless of origin; they do not establish
harmlessness. Do not broaden them or hide other unsuppressed reports.

Qualification has no GitHub waiter. Complete required local evidence is validated
offline; unavailable tools and missing/failed rows remain failures.
Desktop diagnostics retain `build/ai-checks/python-tests.log` when present.

## Git hooks and CI

Install once per clone with `python3 script/ai_setup.py --install-hooks`. `core.hooksPath` is repository-local; installation does not change global settings. Remove this setting with `git config --local --unset core.hooksPath` to restore Git's default hook location. Existing hook installations require deliberate reconciliation, never silent replacement.

Hooks cover `ai-dev`, `ai-main`, and `codex/ai-*`. Pre-commit and pre-merge require the exact Luna receipt plus one fast basic-task check or aggregate candidate check; do not duplicate that predicate. `ai_check.py` strips only `git rev-parse --local-env-vars` from Python-test children; candidate/index checks keep the hook environment. Commit-msg enforces one matching Task-ID; post-commit refreshes ignored task pages. Minor/major candidates need Terra review. Pre-push verifies the checked-out AI ref and clean tree, then runs changed-scope fast checks except on `ai-main` and `ai/*` tags, which require strict checks. Run full build/CTest once at the batch boundary or aggregate closure. Legacy `dev`/`main` pushes are blocked. Hooks are bypassable guardrails, not remote protection.

Publication-ledger validation keeps snapshot planning and finalized change lists
immutable. One exact historical compatibility entry recognizes Snapshot 3 commit
`8df27fb8fa08d9e0cd625b8cad85209fdd09251d`, which predates that rule and
condensed four prose fields. The exception is commit-scoped; later ledgers use
only the standard publication fields.

[Artifact workflow](../../.github/workflows/snapshot-artifacts.yml) builds and packages exact trusted sources for Windows, macOS and Android. It preserves pinned dependencies, provenance and checksums but does not run qualification tests, analysis or emulators. Linux is analysis-only and has no game artifact. Runtime evidence remains a local release gate.

`ANDROID_USER_HOME` and legacy `ANDROID_SDK_HOME` resolve to one run-private preferences directory; `ANDROID_HOME` and deprecated `ANDROID_SDK_ROOT` point to the separate run-private SDK.

Optional `MC_ANDROID_SDK_SEED_ROOT` packages are validated and copied into the
run-private SDK, never shared. Without it, setup acquires only runtime packages;
NDK and CMake remain build-only. Command-line tools stay hash-pinned.

## Project skills and future hooks

The ten [project skills](SKILLS.md) share a deterministic planner. Prefer scripts;
add skills only for repeated judgment outside the [task loop](WORKFLOW.md).

A repository skill uses `.agents/skills/<name>/SKILL.md`, accurate frontmatter,
under 500 words and linked guidance. Test discovery, triggers and commands; retire duplicates.

Provider hooks reuse tested scripts; never mutate code, hashes, task status, or publication. Avoid auto-approval/unrestricted shell; test hooks in a temporary repository.

## Runtime choices

[Project config](../../.codex/config.toml) selects Luna/high for new sessions, subject to overrides. It changes no sandbox, permissions, connections, or global settings. See the [routing policy](MODELS.md).
