# S7 hindsight — setup and execution failures

This appendix records setup failures; the [summary](S7-HINDSIGHT-2026-10-13.md)
gives their causes and corrective order.

## Mac and Windows boundaries

Mac sandbox and native Launch Services views differed: the sandbox saw no
registered application bundles while the native session saw 635. A native
AudioUnit callback worked outside that boundary. We should have tested from the
actual launch context before calling system registration broken. No healthy
global cache was reset.

On Windows, the desktop user could not read the build account’s private
source/dependency trees. An ACL expansion was rejected and never applied. The
correct path keeps build and source inspection in the build account, packages
only runtime files and uses the installed desktop helper for the actual user
session. Its early recipes still had wrong destination basenames, incomplete
PowerShell/tool preflight and command-pattern mistakes. An SCP basename
mismatch overwrote a task-root archive; its bytes were restored from the
retained Mac copy and hashes verified. Previous bytes of two metadata files
were unknown and were not claimed recovered.

A later exact-source transfer exposed a separate macOS packaging defect. The
default tar invocation encoded AppleDouble metadata; Windows tar.exe
materialized ._* sidecars throughout the source tree. The clean Git base was
correct, but the extracted input digest was
91d3a2da305af9bff4f1df2c3e8f8fde8badb86d46b9dcc2d8ec713b27cee733 instead of
the expected 59ed38d976e994267a3ae1466bfef19561fc98c8bf25b7379866fd9393f5da47.
Rebuilding the archive with COPYFILE_DISABLE=1 produced SHA-256
79d95a828343f45307bbabac6f56fce9814d10f67fa7f0037b44b900b5a76262, and the
Windows-side input digest then matched exactly. A cross-platform overlay must
disable AppleDouble metadata and verify
both the archive hash and extracted-file digest before compilation. The first
diagnosis blamed a sparse checkout; that was disproved by git sparse-checkout
and corrected before the build.

The recorder's unsafe-link, lost-cancellation-receipt, and skeletal-terminal
defects now have guards; its focused suite passed 80/80. This does not prove a
new Windows runtime.

A later renderer experiment first used `Start-Process` from SSH instead of the
installed desktop-session launcher. Its read-only `Get-CimInstance` preflight
returned Access Denied, but the nested PowerShell did not inherit the outer
`$ErrorActionPreference = 'Stop'`; it continued and returned a process ID. The
attempt produced zero-byte logs and no benchmark report, so it supplied no
renderer evidence. `windows-exec.sh` now prepends process-scoped stop-on-error
behavior to PowerShell commands. The regression failed before the repair and
passes afterward; on the Windows host the denied query now exits 1 without
reaching a following sentinel, while an ordinary command exits 0. Future GUI
and Vulkan runs use `windows-desktop.py` in the logged-in session.

## Analysis and dependency setup

A narrow analyzer pass over production files missed failures in directly
affected test translation units. Ninja stopped after each first failure, so
retries revealed defects incrementally. The corrected process collected all
125 translation units before repair, then renewed the complete four analyzer
rows with source-matched receipts. Test changes must remain part of that
analysis scope.

The existing Windows Ninja tree also requires the Visual Studio developer
environment. A build launched from bare SSH found `cl.exe` but lacked MSVC's
include paths and failed on the standard `<cstdint>` header. Locating Visual
Studio with `vswhere` and running the same incremental build under
`VsDevCmd.bat -arch=x64` passed; its targeted acceptance CLI test passed too.
The failed attempt was an invocation error, not evidence against the source.

The S7 dependency lock moved CoreCpp and CoreProject to exact local device
selection revisions, but one repository-lock test still asserted the old
revisions and branches. The full 720-test fast-gate run had that single
failure. Updating the stale assertions to match the lock produced a focused
45/45 pass. The lock itself was verified against the retained dependency
checkout heads.

## Command and reporting discipline

One CTest filter omitted ten intended tests, and a runner selected an unpinned
Python from PATH. Source packaging and Windows execution also require real
Git metadata in addition to archived source hashes. A later runner review
caught doubled paths, Git-pointer counting and absent PowerShell parse checks
before transfer or launch. These failures called for a single executable
preflight that verifies tool identity, test discovery, source copy destinations
and one-submit behavior.

Progress updates repeatedly blurred queued and applied model switches, native
completion and visible playtests. The current effort selector was not verified
until a direct helper identity check; its earlier “queued” status did not prove
that the host model changed. Exact process/session and artifact receipts must
precede completion claims, and updates must state phase, elapsed time and the
latest observed result. Long-running tests and benchmarks need bounded
liveness, not invented percentages.

Two S7 handoffs defaulted to primary `ai-dev` instead of the active qualification
tree. Earlier, task discovery ran there but changed no files. On 2026-10-14, a
renderer worker received file ownership without checkout identity and created
three untracked files there; the coordinator removed only those exact files
before integration, leaving the candidate untouched. The first instruction-only
repair was insufficient: a worker could pass a separate preflight and then let a
later write inherit the primary checkout. Write packets now name the canonical
root, branch, expected HEAD, and owned paths. Every mutating shell call must use
that exact `workdir` and assert `pwd -P`, Git root, branch, HEAD, and relevant
status in the same invocation before writing. Patch calls without a working-
directory parameter must use absolute paths limited to owned files. A mismatch
aborts without edits.

Preserve failed receipts; reuse passing ones only when current input hashes
match.
