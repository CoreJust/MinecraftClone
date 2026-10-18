# Qualification preflight and recovery

Before expensive qualification, verify the complete candidate contract in one
preflight: required dependency components and configuration, compiler versus
toolset version, interpreter path, SDK, execution context, and actual generated
artifact paths. Reuse working configurations and owned-process guards; do not
reconstruct them from memory one missing option at a time. On Windows, invoke
hash-verified staged PowerShell files with process-local `-NoProfile
-ExecutionPolicy Bypass`; avoid long inline scripts. Transfer existing receipts
and parse locally, rather than remotely serializing dynamic error objects.
Bound diagnostic queries as well as builds, and verify remote child cleanup.
Before a cleanup decision, retain each surviving owned process's resolved
image, creation identity, parent and Job membership. A denied cleanup remains
a failed supervisor result even if later owned-Job termination reaches zero;
record that terminal containment separately from the native test result.

Retain raw output before parsing. Keep candidate and dependency identities in
distinct variables. A receipt or postcondition failure does not erase successful
native phases: preserve their original evidence, diagnose the failed predicate,
and resume only missing work when exact inputs still match. Never rewrite old
failure receipts into successes. Derived receipts must disclose their source
records and corrected bookkeeping, with the normal exact-candidate review.

### Gate-log authority

`build/ai-checks/*.log` and `summary.json` are mutable latest-run artifacts;
nested test processes may also print an expected failure inside a passing outer
suite. Never classify a current gate by grepping an old log or copied terminal
output. Use the exit status from the exact `ai_check.py` invocation, associate
it with that invocation's `summary.jsonl` record (`invocation_id`, `head`,
`index_tree`, `task_id`, and phase lists), and validate every required phase's
`.receipt.json` against its command, environment, tool identity, relevant
inputs, and evidence hash. A missing or mismatched receipt leaves that phase
unqualified; logs are diagnostic detail, not a pass/fail authority. Keep
intentional nested-failure output in bounded fixture evidence so it cannot
masquerade as the outer test runner's result.

## Early gameplay diagnostic ordering

Start macOS Cocoa/GLFW tests and gameplay in the authorized desktop execution
context, including delegated runs. With `exec_command`, set
`sandbox_permissions=require_escalated` on the outer call; a nested Python
launcher does not change its inherited sandbox. Do not begin with a known
incompatible sandbox probe. If one occurred accidentally, retain it and compare
one bounded same-byte host-context control before diagnosing unavailable Metal,
invalid Launch Services connections, or startup timeouts. Such a failure does
not justify cache deletion, indexing, service restarts, or reboots. Record the
execution context; system-repair authorization remains separate.

Before a sanitizer build, run a small standard-library program and a matching
deliberate-failure control with the selected compiler, headers and runtime.
Check the resolved CMake compiler, not just `PATH`. Empty `main` misses STL ABI
problems. Keep C++ archives sharing container templates consistently instrumented:
mixed libc++ annotations can poison valid slots. Record remaining uninstrumented
libraries explicitly; successful controls do not qualify the game.

For high-risk gameplay, performance or streaming acceptance, run one bounded,
source-bound Release workload as soon as the first candidate can exercise the
criterion. Choose the platform with the tightest relevant target for that
criterion. Exercise prescribed movement and transitions while measuring
continuity; endpoint recovery does not prove continuous coverage. Preserve failed
captures and verify focused corrections against that workload. Independent
functional, analyzer and platform checks may run concurrently on frozen inputs;
do not make a performance pass their prerequisite. Reserve quiet hosts only for
timing. All release gates and the normal Mac hands-on checkpoint remain required.

Test reuse across actual Git hooks. Remove only Git's verified injected `PATH`
prefix; retain environment/tool checks and genuine caller-directory changes.
On Windows verify its MSYS translation before removal. Preserve ordered,
nonempty caller paths and duplicates; do not assume POSIX spelling or empty
component preservation. Exclude only tracing `CODEX_TOOL_CALL_ID` from cache
fingerprints, not child environments; require meaningful changes to invalidate.

Runtime tests must exercise the claimed workload, not only its settings. Speed
modifier tests also verify full input through production physics. Terrain tests
cover current-near admission and readiness during sustained travel and credit
backpressure, not merely eventual loading or stationary captures. Departed
readiness episodes are censored, not counted as successful; existing far terrain
does not establish readiness of newly nearby terrain. Default-radius gameplay
also needs exact current-handle mesh coverage before and after fast travel;
radius-72 screenshots cannot qualify radius 256. Record all stationary attempts,
failures, successful-request intervals and actual phase durations. Timings
conditioned only on successful loops can hide expensive failed attempts; they
are not a frame-cadence measure. Coverage, request throughput and physical display
cadence are distinct claims.

Record the graphics adapter selected by the production presentation path before
interpreting GPU performance. An installed-adapter list or a separate headless
Vulkan enumeration does not prove desktop selection or surface compatibility.
On hybrid-GPU hosts, retain per-device eligibility evidence when the selected
adapter differs from the intended performance device; never label a placeholder
renderer name as hardware identity.

Reserve exclusive CPU/GPU timing windows only after the executable or probe
source and exact command are ready. Prepare source and inspect receipts before
claiming the window; release it promptly after terminal process verification.
An optional probe that is interrupted remains incomplete evidence, even when
earlier focused checks pass. Preserve test counts, commands, source identity
and binary hashes before rebuilding; do not leave the only receipt in a chat
tool transcript.
