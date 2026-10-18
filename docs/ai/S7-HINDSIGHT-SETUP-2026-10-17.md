# 17 October: qualification and setup failures

## Verify the analysis, not just its exit code

The first full ASan game row failed168 tests. Shared libc++ container templates
were instrumented inconsistently across C++ archives: an uninstrumented CoreLang
operation could write a slot ASan still marked poisoned. Rebuilding the consumed
C++ closure consistently resolved these reports; v8 passed729 tests with leak
detection. System frameworks and remaining uninstrumented C libraries are
coverage limits. Disabling container checks would not be a repair.

The v8 scan-build command returned zero despite an internal translation-unit
error. Its replay dropped `-nostdinc++`, mixing SDK and LLVM libc++ headers;
`--status-bugs` did not detect this failure. The corrected launcher preserves
include mode and requires real successful analysis with no internal-error
artifacts. Compiler-only success is rejected; negative fixtures cover this.

Cppcheck rejected an unsupported builtin in compiler-selected libc++ headers.
Its [manual](https://cppcheck.sourceforge.io/manual.html) recommends its own
standard-library model. Omit only those standard headers from Cppcheck arguments,
preserving real compiler commands and project/dependency includes. Do not
fabricate constant definitions for context-dependent builtins.

## Finish diagnosis before renewing long suites

The heading-reset test sampled accumulated arrivals, allowing packet batches
to overshoot its boundary. It now observes the first acknowledged reset metrics
after proving cursor progress, with a bound derived from two256-inspection queue
fills. It passes under MSan; removing the reset condition makes it fail. Client
acknowledgement is awaited separately from the server callback.

After v10 Tidy stopped, another sanitizer renewal began before IWYU had run.
The complete v11 IWYU batch found two missing public includes across127 units.
Collect every configured analyzer's findings before another long sanitizer run.
Earlier failures remain evidence, not waived checks.

## Preparation is not execution

A ready Mac executor repeatedly prepared without running a copy or analyzer.
Root completed the validated894-file overlay in0.14s, copying23 changed files.
The same stall recurred after mesh-probe GO; root's control took0.58s and its
incremental build six actions. Keep ready commands with the coordinator and
report the actual running command; delegate independent work, not another
handoff for an already prepared operation.

The staged exporter deadlocked on875 object IDs: Python wrote stdin while Git
filled stdout. A wait timeout could not interrupt that write. Python3.13/3.14
reproduced it; file-backed transport finished in21–47ms. MC-AI-0466 preserves
bytes, stderr and deadlines. The real export took0.224s; its commit gate passed
714 tests. This was a pipe cycle, not OS-registration failure.

## Validate inputs and consumers before building

A Linux overlay matched frozen files but retained an untracked `.orig` backup
inside the checker's `tests/**` input glob. Passing rows described different
inventories. Preserve backups outside qualification sources and compare the
complete selected path/size/hash set; never rewrite old receipts for equality.

Windows CoreCpp lacked audio components; Android lacked executors. Producer
tests cover enabled components, not every consumer requirement. Probe the
complete consumer set, then build missing components at the same revision into
a separate prefix. Do not disable features or demand clean worktree status from
an intentionally frozen overlay. Verify baseline, index and inventory instead.

## Account for all retained outputs

The user reported over500GB and three million files in the Windows profile.
Available evidence measures only two S7 roots:10,467,992,097 logical bytes
(9.75GiB), with many older roots unmeasured. They retained source/build
generations, prefixes, staging and raw proofs. The desktop launcher also copies
each package into a new ProgramData job without retiring old jobs. This does not
prove a whole-profile copy or explain the second similarly named profile.

Retaining generations without an aggregate lifecycle was the workflow failure.
Deleting selected producer objects ignored old roots, proofs, package copies,
temporary storage and caches. The1–2GB limit applies to their sum, not per agent.
Windows remains untouched; no account restoration or cleanup is inferred.

A private producer successor pins scope, inventory and measured peaks, using
one fixed lock. Review reproduced a caller-selected-manifest serialization
bypass; the repair rejects it. Its approval pin remains unset: no real jobs are
enabled. Tested admission is not an OS quota or proof the host fits2GB. Future
use requires actual inventory and verified export before scoped retirement.

## Respect CWD and observation boundaries

The supervisor accepted CWD but omitted it from Popen. ASan preconfigure read
another checkout's presets and failed before testing. Explicit outer CWD let
v23 finish; a separate successor fixes forwarding. Four harmless controls cover
wrong/missing CWD and preserved arguments, limits and containment. No OS repair
was involved.

The0457 commit hook also failed its real process-identity test inside the
sandbox; the unchanged candidate passed in the host context. Run native
identity and desktop checks in that context initially; this was an invocation
mistake, not another macOS defect.

A fake clock fixed at511 never reached the revised540s deadline: mock calls
grew to10GB. The unwrapped test outlived240s, and compaction lost its start.
Bound processes before launch, persist start/expiry, and print test names.

The v3 observer rejected normal closing animation and repeatedly activated an
active window. Reviewed v4 activates only until armed and stops observation at
explicit native completion. Mid-measurement checks remain strict. Its next run
terminated normally but genuinely missed two of721 movement slots: not an FPS
pass. Preserve raw events and diagnose before changing or retrying the game.

Basic commits own implementation;0038 owns final qualification. Windows is
deferred only for S7, not passed; Mac, Android and CoreLang requirements remain.

Zeroed archive mtimes hid changed inputs; refresh them. Mismatched Ninja
diagnostics rewrote Android's cache; use the configured executable.

Further runtime findings: [runtime hindsight](S7-HINDSIGHT-RUNTIME-2026-10-17.md).
