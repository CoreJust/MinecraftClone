# S7 delivery hindsight — 12 October

This supplements the [11 October findings](S7-HINDSIGHT-2026-10-11.md). S7 remains unreleased.

The normal Windows playtest exposed distant-first terrain despite green
stationary captures and eventual-loading tests. Those checks did not measure
current-near progress during repeated movement and backpressure. The x200
benchmark also supplied only 1/127 of full axis input: a modifier assertion was
not evidence of genuine full-speed travel. MC-AI-0442 corrects both mechanisms
and tests production physics, near admission, and readiness episodes. MC-AI-0443
fixes required analyzer diagnostics without suppressions. Neither the earlier
benchmark nor a sanitizer pass qualified the user's rejected playtest.

Two task-owned Windows log queries consumed about 21 GiB and blocked shared
builds. They serialized `failure.error` with `ConvertTo-Json -Depth 8`; expansion
of that object is a suspected mechanism, not a measured allocation diagnosis.
Native handle-backed command queries established exact task paths, read-only
operations and creation identities. Cleanup rechecked command hashes and
creation times on the same handles, terminated only those two queries, and
confirmed both exits. Physical availability recovered to 28,768,518,144 bytes.
Future queries must share builds' bounded ownership and cleanup; local SSH
cancellation is not proof of remote exit.

The Windows Release recipe then omitted required dependency component options,
failed to pin the already installed Python interpreter, assumed the wrong
executable output path, and shadowed the candidate SHA with a dependency SHA.
Repeated inline-command length and execution-policy errors added avoidable
delay after their causes were known. These were setup and receipt defects, not
new game requirements. The corrected run preserves each failure, reuses its
successful native phases, and resumes only missing work. CTest passed 607/607;
the final receipt correction remains distinct from that native result.

The Windows production-client capture subsequently passed its full-radius-72
and 16-heading assertions and produced a retained labeled image. Native Job
Object readback confirmed the agreed 6 GiB cap and zero owned descendants after
cleanup. This qualifies automated capture only, not hands-on acceptance or FPS.
Preparation of the remaining analysis wrapper still exposed incorrect setup
exit propagation; review corrected those branches before execution. Qualification
scripts need tested failure paths, not just successful command paths.
The first analysis launch also treated Git's informational stderr as a
terminating PowerShell error; recovery verified and reused the clean checkout.
Its successor exposed the same Python-discovery omission already repaired in
the normal Release recipe: pinning the runner interpreter did not expose it to
CMake. This recurrence shows that fixes were not consistently transferred into
the shared qualification recipe. Phase-local tool discovery must be validated
once and reused across rows, rather than rediscovered after each failed launch.

The resumed ASan row compiled successfully but its legacy 1200-second matrix
deadline interrupted CTest after 517 of 607 tests had passed. No failure was
recorded; the result remains incomplete. MC-AI-0444 changes only that row's
budget to the already agreed 2400-second bounded slot. The checker validates
all nine original Mac/Linux receipts unchanged against the corrected manifest;
only ASan requires a fresh full run. The wrapper also mislabeled its build root
with underscores where the preset uses hyphens. The runner used the correct
path, but reuse now checks the actual cache and outputs rather than that label.

The corrected-budget ASan run completed all 607 tests with zero failures and
all three commands returning zero. Its supervisor nevertheless failed closure:
two descendants survived natural drain, and one resolved image did not match
the narrow cleanup allowlist. The helper terminated only its owned Job and
read back zero processes before closing it. The failure record omitted the
denied image and creation identity, so its exact identity cannot be recovered.
The native test pass and failed supervisor receipt remain separate; the
allowlist was not widened and the suite was not repeated to erase that failure.
The normal MSVC analyzer run likewise completed all 607 registered cases with
zero failures, including one expected Release-only skip already passed in the
normal Release suite. Its supervisor hit the same closure predicate and then
verified its owned Job empty. Both original failure records remain retained.

These failures show why another explanation alone is insufficient. The
[qualification preflight and recovery rules](QUALIFICATION.md)
now require a complete preflight, staged invocations, retained raw output,
separate candidate/dependency identities, bounded queries and input-verified
phase recovery. This is a documented repair supported by direct evidence, not
a guarantee that all setup defects are eliminated. S7 publication and exact
package acceptance remain open.

The 13 October source commit passed its full hook gate, but the subsequent
proof-only gate rejected all four phase receipts. A real Git-hook regression
reproduced the duplicate execution. In the coordinator's normal-host context,
adding only Git's execution-directory prefix to `PATH` reproduced the exact
stored hook fingerprint; direct checks had a different fingerprint. Existing
tests simulated hook variables but omitted this real hook behavior. MC-AI-0445
addresses that execution boundary without ignoring genuine environment drift.
Its acceptance requires a real-hook reuse regression and negative environment
checks; the original failed proof and passing native evidence remain retained.
