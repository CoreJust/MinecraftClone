# S7 native-analysis follow-up — 11 October

S7 remains unreleased. This extends the [delivery hindsight](S7-HINDSIGHT-2026-10-11.md).

The Release ASan preset now emits link symbols as well as compiler symbols.
Without a linked PDB, the first heap-error report had addresses but no owning
source lines; relinking with `/DEBUG` identified the exact self-child type
assignment. The preset regression checks this diagnostic prerequisite.

- **Native discovery was still too serial.** The corrected dependency closure exposed missing `/EHsc`, then ASan's required debug-information flag and MSVC's object-section limit. `/analyze /WX` found deprecated shared-pointer atomic calls. The nested package consumer selected Visual Studio instead of its enclosing Ninja generator and reported an external fmt warning. Generator selection alone did not fix it; explicit external-header analysis policy did. An owned-source negative control still failed with C6240 and C2220. Batch configuration, compilation, package-consumer and runtime discovery before freezing another candidate; retain completed dependency caches between corrections.
- **Local and hosted analyzer versions differed.** Earlier local IWYU validation used 0.27; the hosted gate used 0.26. An upstream 0.26 replay reproduced its recommendation for nonexistent `<math>` instead of `<cmath>`. A complete scan exposed additional usable public standard-header dependencies. The corrected adapter and include batch subsequently passed 125/125 translation units. Record analyzer versions alongside source and compiler context. Retain raw per-file diagnostics separately from validated matrix receipts.
- **Windows handoff details were not authoritative.** A copied BuildTools path omitted `(x86)`. Bootstrap commands encountered missing cache directories, child-shell execution policy and Python/compiler PATH ordering. Actual host readback established the usable paths. Consolidate those prerequisites and verify environment preservation before dependency builds. These errors added avoidable work; they do not explain the whole historical delay.
- **A complete scan exposed the actual warning batch.** Ninja keep-going attempted all remaining analyzer objects and reported 406 warning instances. All 376 constant-comparison reports mapped to Google Test assertions; none were in production. The remaining batch included shadowed names, deprecated environment access and excessive stack buffers. Test-framework diagnostics must be distinguished from owned defects without globally disabling analysis. Mandatory native rows remain open until the corrected complete build and tests pass.
- **Successful sanitizer compilation was not runtime qualification.** The instrumented Windows dependency closure and full game build passed, but runtime tests reported stack-buffer errors in CoreLang's fiber-based compiler entry. A paired-notification native prototype passed 1,000 switches, stack growth, thread/TLS, exception and existing-fiber cases. Its intentional invalid stack write still failed with ASan. The reviewed production correction subsequently passed its four focused native tests; the evaluator's 53 cases also passed with its tracked stack reservation. A separate runtime tail-call stress timeout remains recorded outside the game matrix. Neither those focused successes nor prototype evidence qualifies the full game row. No sanitizer suppression or earlier source receipt substitutes for that evidence.

The clean maintenance packages subsequently installed on all three target
platforms. The next complete Windows run exposed another closure boundary:
the nested game server-only build dropped compiler and linker flags, causing
ASan annotation mismatches. Its template now forwards those settings, with a
regression covering the contract. A disconnect fixture also stopped polling
before its queued observer notification could flush; the polling lifetime is
corrected without changing the deadline. Other native timing failures remain
under investigation; no timeout increase or successful matrix is claimed.

Serial replay separated some contention from repeatable failures: five of seven
ASan runtime failures passed unchanged, while the capability-cycle fixture and
the exact radius-256 disk-difference oracle still exceeded their deadlines.
The latter also took 72.24 seconds without ASan. Its oracle materializes twelve
complete interest sets and inserts 1,543,764 keys into node-based sets before
the difference comparisons. More importantly, it released the nondefault
radius-128 prepared orders after every call, forcing all sixteen sorted heading
tables to be rebuilt six times. Retain those public prepared orders for each
radius, as the production server does, without shrinking the oracle's radii,
transitions, exact-set assertions or sanitizer coverage. The corrected case
passed natively under ASan in 28.68 seconds with the unchanged 120-second cap.
The cycle, disconnect and nested instrumented build checks also passed natively
in a focused three-case run. Complete native rows remain separate gates.

Two handoff errors added avoidable work. A bare preset reset the shared vcpkg
prefix, causing a nested Mac configure to report missing GLM; restoring the
override made the build pass. Another worker duplicated roughly seventy seconds
of broad tooling checks despite a focused assignment. Preserve configure
overrides and assign one gate owner; neither incident warrants global changes.

A model-picker interruption stopped the Windows monitor, not its native ASan
command. A status message did not resume it; a follow-up task recovered the
existing session without duplication. Inspect both agent and command state
after interruption. An active command does not establish an active monitor;
no system repair was required.

The sequential analyzer row subsequently passed all 596 registered cases, with
one existing optimized-build-only skip, in 246.70 seconds. The full ASan row
failed three timing/acknowledgement cases in 717.71 seconds; all three passed
unchanged serially in 38.64 seconds. The projection timeout overlapped nested
compilation; an acknowledgement failure did not. Scheduling alone cannot
explain every failure.
The nested build fixtures should not multiply runtime-test parallelism with
their own compiler fan-out. The two physics-only fixtures also request no
previews, yet each process prepares the full default terrain-order radius;
using the minimum valid unused preview radius preserves their cold physics
conditions and assertions. Both corrected native rows passed. Exact-commit
hosted qualification remains required; diagnostics never replace full rows.
