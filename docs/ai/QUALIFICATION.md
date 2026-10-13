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

Qualification reuse must also be tested across the actual Git hook boundary,
not only with manually constructed environment dictionaries. Git prepends its
execution directory to hook `PATH`; restore only that injected prefix before
launching checks, while retaining ordinary environment and tool-identity checks.
Never ignore arbitrary `PATH` changes or replace a failed proof with a claimed
pass. Git for Windows translates its execution prefix through MSYS; verify that
prefix before removing it. Other shell-added directories remain genuine
environment changes and require checks to rerun. Tests preserve the ordered
nonempty caller directories, including duplicates, rather than assuming POSIX
path spelling or empty-component preservation.

Runtime tests must exercise the claimed workload, not only its settings. Speed
modifier tests also verify full input through production physics. Terrain tests
cover current-near admission and readiness during sustained travel and credit
backpressure, not merely eventual loading or stationary captures. Departed
readiness episodes are censored, not counted as successful; existing far terrain
does not establish readiness of newly nearby terrain.
