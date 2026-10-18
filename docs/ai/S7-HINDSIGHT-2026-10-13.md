# S7 hindsight — 13 October checkpoint

Current requirements and platform deferrals: [qualification](GAMEPLAY_QUALIFICATION.md).

On 13 October, S7 remained unreleased. Its 15-day duration was caused by failures in scope
control, representative testing, setup preflight and evidence sequencing. The
feature set is substantial, but there is no evidence that the work inherently
needed fifteen days. The earlier “about two days” estimate was also
unsupported: we did not track effective engineering time well enough to justify
it.

## The central mistake

We treated component tests, smaller-radius captures and successful builds as
evidence that the game was nearly ready. They did not exercise the default
256-chunk world after fast travel. On Windows, the full-radius gate first
stalled at a renderer capacity boundary, then completed coverage while failing
the requested cadence by a wide margin. The user’s playtest independently
reported missing terrain rings and 11 FPS. We should have made the real
full-radius travel workload a promotion check before asking for another
playtest.

The later stage-level Windows trace localized a concrete freeze to tile removal:
one frame took 88.073 seconds after 128 removals, with 88.071 seconds spent
refreshing adjacent seam meshes synchronously. Removing the renderer entries took
1.237 ms total and queue bookkeeping 23 μs. The implementation now defers those
four-neighbor rebuilds to the prioritized mesh queue; an exact-source Windows
rerun is still required to show whether this removes the stall and terrain holes.

The qualification contract also grew beyond its source. I turned the Windows
500 Hz goal into a hard snapshot gate before tracing it to the requested
release boundary. The owner clarified S7 as complete radius-256 terrain and
stable 120Hz at100+chunks on both desktops, with500FPS deferred. These are now
separate radius-256 coverage and radius-128 performance proofs. The correction
does not authorize lower terrain distance or visual quality.

The latest exact-source diagnostic exposed another acceptance defect: raw
movement frames already carried current-near terrain counts, but the full-radius
terrain result was completed from its initial and post-recovery endpoint
barriers. The offline verifier ignored those in-motion near samples and treated
radial bands as valid whenever their counters reconciled, even when 637 tiles in
the nearest scanned band lacked current meshes. The same run later refilled the
endpoint after 177.8 seconds, which made the terrain sub-contract say “complete”
despite the observed holes. We are adding per-loop movement/recovery coverage
accounting, offline checks against every recorded movement frame, and explicit
radial-band completeness checks. This is a concrete test-design failure, not an
inherent cost of the feature work.

## Why the loop repeated

Several fixes were followed by broad platform/analyzer renewals before the
most expensive Windows workload was settled. Each newly found engine issue
invalidated those receipts. At the same time, narrow checks missed direct test
translation units and headers; a build stopped at its first analyzer failure,
so later failures surfaced only after repeated runs. Exact source, dependency,
tool and runtime identities were not bound consistently until late in the
process.

Setup assumptions added further retries: sandbox-only Mac failures were
misdiagnosed as host damage; Windows build and desktop accounts could not share
private source trees; and the runner recipe had wrong copy names and
insufficient preflight. Those were integration and command-discipline defects,
not reasons to change system permissions. We restored overwritten transport
data from retained evidence and did not broaden ACLs.

Progress reporting also failed. Work was described as “queued” or complete
before the live model, exact process, package or user-visible game was
verified. That forced the user to discover missing work after long waits.
An estimate, heartbeat or green test cannot replace an observed terminal
receipt.

## Evidence and present state

The 16 October v7 Windows run kept near terrain complete throughout6100 samples
and completed its R128 barriers in11.137/4.878s, but failed performance at
99.744% in-budget time against99.8%. Background CoreLang ASan was declared;
neither a quiet-host pass nor a causal explanation is established. Mac v5 passed
request cadence, but v7 still needs its own proof. Earlier GPU experiments and
failures remain in [the technical appendix](S7-HINDSIGHT-TECHNICAL-2026-10-13.md).

V7 Linux analysis-only LSan/MSan passed348 tests each; Mac UBSan passed729.
Subsequent v8 Mac ASan/leak detection and TSan each passed729 tests; the paired
Android APK installed and rendered terrain. V8 MSan failed a timing-sensitive
test, now replaced by a mutation-proven reset regression. These are separate
source-bound results, not current-candidate release readiness.

## Changes required in how we deliver

1. Freeze the user-derived release contract and exact default workload first.
   Coverage, visual equivalence, cadence, platform runtime, package identity
   and screenshots are separate predicates.
2. Use the full-radius moving workload early, before a playtest or expensive
   platform matrix. Preserve every failed run and measure the actual device.
3. Collect diagnostics across all affected production and test translation
   units in one analyzer pass. Preflight host, account, copy destinations,
   exact tool paths and test selection before reserving a long run.
4. Run independent checks concurrently on frozen inputs; a performance failure
   is not a prerequisite for unrelated qualification. Reserve quiet hosts for
   timing and reuse receipts only when checkers prove unchanged inputs.
5. Report observed commands, session IDs, source hashes and outcomes. Never
   describe a queued model or pending job as applied or complete.

The [technical appendix](S7-HINDSIGHT-TECHNICAL-2026-10-13.md),
[setup appendix](S7-HINDSIGHT-SETUP-2026-10-13.md), and
[15 October setup addendum](S7-HINDSIGHT-SETUP-2026-10-15.md) and
[17 October analysis repairs](S7-HINDSIGHT-SETUP-2026-10-17.md) and
[18 October streaming QA](S7-HINDSIGHT-STREAMING-2026-10-18.md) retain the
measured engine timeline and environment/workflow evidence. S7 remains active
until all required checks, hands-on acceptance, requested artifacts and
authorized publication are actually complete.
