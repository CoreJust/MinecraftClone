# 18 October: the moving-terrain QA gap

## What the tests failed to establish

S7 checked full terrain before and after travel, but its continuous movement
check covered only the nearest225tiles. The user saw incomplete waves beyond
that area. Both checks could pass while most requested meshes were missing.
This was a coverage-design error, not a machine-availability explanation.

The first real broad-radius diagnostic exposed51433initial meshes collapsing
to roughly5000current meshes during x200 travel. Later endpoint recovery did
not repair or invalidate that observation. Finite queue tests also missed the
continuous-arrival case: eventually draining a fixed backlog does not show
that new chunks are served before they leave a moving interest region.

## What the investigation established

Boundary counters separated server generation, credited delivery, residency,
mesh admission/publication and GPU-slot retirement. They exposed distinct
bottlenecks: unflushed application credits, raw-packet credit underutilization,
client count ceilings, an insufficient background generation allowance,
incremental ranking barriers, stale-head maintenance and lagging removals.
Each correction has focused regression evidence and retained real-flight
comparisons. They improved throughput, but improvement was not completion.

Per-key episodes then showed40354of516443newly requested chunks departing
before their first publication. Of those,23984had waited over one second.
The sampled gaps were first-publication failures, not merely later remeshing.
That evidence keeps moving-terrain quality open despite fast final recovery.

The first class-priority correction stopped at pending admission; refresh
maintenance and worker/result selection were separate policies. Fixing one
boundary therefore did not establish end-to-end service. Even128outstanding
jobs can wait indefinitely when closer rebuilds continually overtake them.
Residual telemetry disproved the assumption that the remaining long waits
were mainly missing data. Tests of an unused public deferral helper also do
not establish the behavior of the private production deferral path.

## Process correction

Test the user's visible failure before renewing platform matrices. Exercise
continuous arrivals, departures, priority changes and rebuild competition,
not only isolated or finite queues. Preserve censored work in latency reports;
excluding chunks that never appeared would bias the result toward success.
Use broad quality diagnostics separately from low-overhead FPS qualification.
Trace priority through every ownership handoff, and exercise the selectors
actually used by queued work, completed results and deferral. A capacity bound
is not a waiting-time guarantee.

Every proposed correction must name its failing contract and changed real
behavior. Do not raise budgets blindly, lower radius or quality, or declare
success from a near-only probe. Current receipts and limitations are indexed
in the active delivery checklist and the0469evidence root; this hindsight is
not a release receipt or a claim that the remaining defect is fixed.
