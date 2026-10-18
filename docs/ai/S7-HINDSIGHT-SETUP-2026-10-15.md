# S7 setup addendum — 15 October

This records current Windows and macOS setup evidence after the earlier
[setup review](S7-HINDSIGHT-SETUP-2026-10-13.md). It does not close any
runtime gate.

## Windows access and dependency identity

The documented direct-LAN `windows-build` SSH alias answered a read-only
preflight as `desktop-vv19svu\codexbuild`. A separate
`Get-CimInstance Win32_OperatingSystem` query returned Access Denied inside
that account. That is a command-permission boundary, not proof that SSH is
down; stop diagnosing transport once the authenticated identity succeeds.

A retained configure log at
`C:\Users\codexbuild\CodexTasks\s7-near-admission-20261015\evidence\configure.stderr.log`
(SHA-256 `5dfcad568d56d41c49a254e4ad2a2cc16f96f57e9c418ddefb6a8da510980f93`)
shows CMake rejected the package prefix before compilation. The candidate
requires CoreCpp `03a3f4ecfb00128fe8c7b14141e12c853a89b48c`; the prefix exposed
`c5d5cffe9ed2ccce478db3426e2ae185df7cfe40`. This is stale dependency state,
not a source failure. Acquire the locked package pair into a clean,
task-owned prefix and verify provenance before configuring. Never disable the
mismatch check to get a build started.

A later clean source transfer (archive SHA-256
`c9b03d72b0a280e1607a3b86abcea61c076e00e087ae7eb3bf7e979ca18a4bb1`)
configured successfully with the locked CoreCpp `03a3f4ec` and CoreProject
`e660b422` prefixes, MSVC 19.44, and task-local Python 3.12.10. This was only a
configure preflight: the host Vulkan SDK was 1.4.341.1, not the pinned 1.4.357.0
toolchain. A second native build was active during the first check, so I
withheld compilation then; after it became idle, I resumed with one below-normal
job pinned to one processor and a 1 GiB process-set ceiling. The Release
`mc_main` rebuild completed with zero `C4996` warnings after replacing the
benchmark's deprecated `getenv` calls with `_dupenv_s` on MSVC. The rebuilt
executable SHA-256 is `dc54a258a83c269a9cf0a21c53e5811f7641283bc3072be781af1dbdd6d42607`;
it took 8.1 seconds with a 425.2 MiB tracked peak. The build log SHA-256 is
`f6925bce7e9a547db7b0b6c235e13b913682e3b43a6509d9218733ff6fc0d407`. The
two-file source overlay SHA-256 is
`a8acb80a71ff8e65f2e072d44128fce5f7bd173cadc381a4d0de92d37324e7cf`.

The same source copy then compiled all 84 `mc_tests` build steps without
warnings; peak tracked working set was 405.6 MiB over 215.8 seconds. The log
SHA-256 is `5c036c803c80b07a0c02b1623d09fe3d9b36543313cdedc689a2f8c68bc147dc`.
The Windows executable passed 25 focused tests: 24 `GameBenchmark` cases and
`GameServerPreviewTest.RepeatedCenterTravelRestoresNearTerrainAheadOfQueuedBacklog`.
No game or benchmark was launched. These checks validate MSVC compilation and
deterministic behavior only; the Vulkan SDK mismatch and Windows runtime,
rendering, and cadence gates remain open.

After that change, the current worktree passed native CTest 688/688 in 145.63s
(log SHA-256 `86b7b9db5f890c7bb58db0b9ceb04efdbf073e8bf61266e09e0392e38ef8f877`)
and UBSan CTest 688/688 in 161.64s (log SHA-256
`1e14f0522d7d9c8b32593aedb91dee200f5d95747f3f9373e58caee95180ec5f`). The
fast local gate also passed all 729 Python tooling tests (one skipped). These
are deterministic/local checks, not Windows gameplay or cadence evidence. The
build account's `tasklist /FI "IMAGENAME eq mc_main.exe"` returned `ERROR:
Access denied`; `query user` returned `No User exists for *`. I therefore could
not establish whether your desktop session was using the GPU and did not start
a GPU-heavy run.

The existing Android AVD is `emulator-5554`, confirmed as
`Medium_Phone_API_35` / API 35 / arm64-v8a; every `adb-agent doctor` subcheck
passed. The foreground activity was `com.yandex.maps.testapp`, while the
MinecraftClone package installed on that emulator reports version `0.1.0:7`.
I did not foreground the game, replace its package, stop the other app, or
create another AVD. Reuse the shared emulator only after its current owner has
released it; the read-only inventory and doctor checks do not constitute a
fresh S7 Android runtime test.

## macOS launch boundary

The historical OS-level diagnosis below is superseded for the current startup
failure by the [controlled 16 October comparison](S7-HINDSIGHT-SETUP-2026-10-16.md).

The game launch stalled in GLFW/Cocoa initialization before renderer readiness.
The later native Launch Services dump had no registered bundles; `lsregister
-seed` and explicit Terminal/iTerm registration both failed with `-10822`
while Spotlight remained disabled. A current-user cache rebuild included one
reboot but did not repair registration. No further macOS reload, reboot, cache
deletion, or service restart is permitted for this qualification task. Treat
this as an OS-integration blocker, not game rendering evidence; preserve the
failed diagnostics and continue all unaffected work.

After that reboot, one current Release `capture-render` probe failed at its
10-second watchdog. The retained report
`build/ai-checks/s7-startup-smoke-20261015.json` has SHA-256
`997a23467d19d39c7d51d5b44e059a696c0b9c2659519e52b7471fb9fc1c0b79`; no image
was produced. Its console output reports an invalid
`com.apple.hiservices-xpcservice` connection and `_LSModifyNotification`
failure. A fresh database dump still has zero `Bundle` records. Post-reboot
`lsregister -seed` and forced registration of the exact Terminal/iTerm bundles
both fail with `-10822 from spotlight`. Spotlight remains disabled by user
instruction, so stop OS-level repair attempts here; no restart or alternate
global setting is authorized.

## Process correction

For future platform attempts, first record transport identity, command
permissions, dependency revision and build-tree provenance. Only then reserve
a full compile or runtime session. If Launch Services needs system-level
repair, do not treat a restart or cache deletion as an automatic playtest
workaround; keep the gate open until that repair is separately authorized.

Gate diagnosis must likewise start from the exact invocation, not whichever
mutable log happens to be on disk. The current Python runner can contain nested
fixture output, so a child `FAILED` line is not the aggregate result. Bind the
captured exit status to its `summary.jsonl` invocation identity and validate
required phase receipts before reporting a gate failure or pass; use logs only
to diagnose the identified phase. The qualification guide now records this
rule, and the targeted TSan fixture stores its intentional child failure in a
bounded evidence file.
