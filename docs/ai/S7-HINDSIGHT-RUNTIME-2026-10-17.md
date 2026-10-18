# 17 October: runtime diagnostics and fixture contracts

## Exercise diagnostic shutdown before a long sample

The temporary mesh probe registered its exit callback before constructing the
function-local state. Reverse destruction order destroyed that state's mutex
before the callback locked it, aborting after the gameplay sequence and losing
the buffered rows. This is an instrumentation defect, not a demonstrated
shipping crash. Construct the state before callback registration and exercise
that exact lifetime order in a short control before another long probe. Retain
the failed binary/log/raw report; do not relabel its result as a pass.

## Template discovery failure is not proof that the tool is missing

Xcode's template listing omitted Time Profiler, but its bundled template worked
by explicit path in host context. A two-second control recorded and exported
successfully. The sandbox's sampler denial was also context-specific. Resolve
the selected tool's actual resources and execution context before proposing an
installation or system repair. The default Time Profiler captures CPU stacks,
not a complete waiting/descheduled-thread history; retain that evidence limit.

## Recover the result before repeating interrupted work

After the observation sessions disappeared, the Windows v13 performance job
had already succeeded. Recovering its original receipt, raw report and exact
desktop sidecars established the result without another build or launch. The
raw predicate and complete transport validator both pass. A missing tool handle
does not establish that the underlying job failed or that its evidence is lost.
Recover the artifact paths recorded by the receipt, including the packaged
runtime and descriptors, rather than copying only the top-level JSON.

## A copied diagnostic executable is not a staged runtime

The first isolated diagnostic launch failed before connection because the
copied executable lacked adjacent shaders. The normal release package was
complete; this was a coordinator-created diagnostic staging error. The corrected
attempt copied the verified shader set. The diagnostic runner now validates the
original package manifest and adjacent shader bytes before launch. Keep that
startup failure, the diagnostic binary and its patch separate from normal
runtime qualification. Restore temporary source edits byte-for-byte and rebuild
the production target: restoring source alone does not replace diagnostic object
files or the executable in a reused build cache.

## Do not tune from an unlocalized wall-time spike

V13's failed Mac capture contains both an 8.292 ms mesh-pump outlier and
a 16.429 ms terrain-interest outlier. Lowering the mesh budget cannot explain
or bound the latter. A buffered subphase/thread-CPU diagnostic did not reproduce
that 16 ms call; its maximum was 2.606 ms wall and 2.605 ms CPU, mostly removals.
This is not evidence that a lock or scheduler caused the older stall, nor a
qualification pass. The longer diagnostic sample is evidence gathering, not a
change to the release workload or its 99.8% threshold.

## Keep isolated mesh fixtures separate from full-interest admission

Replacing arbitrary interest sets with a geometric disk exposed an invalid
test setup: its independent center previously prevented normal near admission.
The radius-1 fixture now correctly admits five tiles, so waiting for exactly
one completed job fails despite successful worker execution. The host probe
recorded five results and five outstanding jobs. Single-job publication tests
must explicitly submit their target and suppress additional admission while
draining it; the travel-order test must still exercise real result priority.
Do not weaken production near coverage or reinterpret these counts as a
connection or operating-system failure.

## Reconcile presentation history by generation and request

The first driver-history collector treated presentation IDs as globally unique.
Normal arena growth recreated the swapchain during warmup, resetting those IDs;
the collector rejected valid measured history before evaluating cadence. Key
requests by generation and ID. Permit only initial-warmup generation advances,
lock the generation at the first baseline attempt, and require every request in
that measured generation to have attributable, increasing driver timestamps.
Missing old-prefix history must not excuse a missing measured request. Native
and verifier regressions cover both cases. V16 passes the separate Mac R128
performance and R256 coverage proofs; failed earlier captures stay failed.

The supported asynchronous MoltenVK submission setting is now the normal Mac
default, with explicit overrides preserved. The qualification result uses
driver-reported presentation timestamps, not the noisier CPU-completion proxy;
it is not an independent physical-scanout measurement.

## Run the native control in its required host context

The first v16 ASan row failed before game tests: sandboxed CoreAudio could not
find the output component, then sanitizer symbolization reported an8102-byte
demangler leak. One host-context control passed in2.56s with identical source,
compiler, runtime and leak settings; its real callback and intended4096-byte
leak were recognized. Rerun the failed row in host context, preserving the
failure. Do not add a suppression, disable leak detection or repair macOS.
# 18 October: repeated validation from tracing identity

Two host-context probes differed only in `CODEX_TOOL_CALL_ID`. Native source
and tool fingerprints matched, but including that per-call tracing ID in the
environment fingerprint prevented receipt reuse. The repair excludes only that
key from the fingerprint, leaving child environments intact. A regression first
reproduced the miss, then passed; changing `ASAN_OPTIONS` still forces execution.
Two real host calls now produce the same normalized fingerprint. Existing
receipts are not rewritten; fresh normal validation is required once.
This explains the repeats observed today, not a measured share of the full delay.
