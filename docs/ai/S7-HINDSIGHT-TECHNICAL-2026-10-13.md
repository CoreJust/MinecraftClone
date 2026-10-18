# S7 hindsight — workload and engine evidence

This appendix records why passing local tests and partial world captures did
not predict the full-radius result. Read it with the [hindsight summary](S7-HINDSIGHT-2026-10-13.md).

## Coverage failures before timing

At radius 256 the desired disk contains 205,861 terrain tiles. The first
Windows full-radius run remained around 55,268 current meshes from 30 to 135
seconds because resident face count sat just below the 16,777,216-face arena
limit. Coverage resumed after growth. A later short run crossed the same
boundary but still did not establish full completion. Those were capacity and
progress failures, not valid FPS samples.

After the capacity and adapter-selection fixes, the RTX 5070 Ti Laptop run
completed initial and recovery coverage in 134.084 and 152.099 seconds. It
still missed the requested Windows cadence at about 77.9, 77.8 and 114
requests/s. User testing later reported terrain gaps during travel and 11 FPS;
the automated stationary result did not cover that experienced scenario.

Mac evidence also separated coverage from speed. A candidate completed
205,861-mesh barriers in 128.800 and 145.937 seconds. On a dense specimen it
submitted about 12.5 million quads; directional culling reduced one workload
to about 8.4 million. Cadence remained far below the Mac goal. A later LOD
certificate reduced baseline geometry to 7.44 million quads and improved
recovery, but still measured only 55.0/78.49 requests/s. It preserved the
2 px² face-area bound; it did not close the performance target.

## Measurements that changed the hypothesis

One Windows diagnostic attributed about 6.2 ms to range collection and about
13.8 ms to center-change work. Packed index storage and stable ordering reduced
recovered collection from 6.572 to 2.271 ms, but the next full-radius run
remained at 55.26 baseline and 114.47 recovery requests/s. CPU locality work
was useful yet insufficient.

The 0458b RTX run measured dense baseline GPU terrain time at 12.843 ms. Its
recovered scene had more logical draw ranges but about one quarter as many
quads and about 3.55 ms GPU time, so draw count alone was not a useful
performance proxy. A one-pixel scissor left dense time at 12.831 ms; fragment
fill was not the dominant cost in this scene. Replacing the shader corner
array with a packed decoder measured 12.836 ms, providing no useful speedup.

These are diagnostic terrain timestamps, not whole-frame duration or physical
scanout. Coverage, CPU costs, GPU work, presentation requests and the user’s
observed experience remain distinct evidence. None of these measurements
qualifies stable 500 Hz. The 4096-quad indexed-block path must first demonstrate
image parity across legacy modes, seams, holes and wrapped camera positions;
only an exact-source Windows A/B can establish whether it improves this
workload.

## Why the earlier gates were weak

Radius-72 screenshots and near-world readiness tests never established
full-radius residency after fast travel. A coverage barrier alone then proved
all meshes existed but did not prove speed. Some test commands ran before the
final header or test-file changes. The first analyzer retry stopped at its
first failing translation unit, hiding later failures. A whole-source
collection finally checked 125 translation units with 250 tool invocations;
the completed analyzer rows and 98 focused tests passed, but still did not
prove Windows runtime performance.

The promotion gate now rejects missing full-radius barriers, wrong device,
source/dependency drift and inconsistent request timing. This makes the
evidence more trustworthy; it does not make a failed game workload pass.

The x200 recovery path then exposed a separate server-side starvation defect.
With 57,947 obsolete removals queued and client credits stalled, the server
filled bounded delivery-operation slots with removals before admitting newly
near tiles. A production-path regression reproduced the first near tile only
after 798 operations, against an 80-operation bound. The scheduler now mixes
bounded removals with current-interest additions while respecting residency
capacity; the focused server/client preview suite passes 37/37. This proves
the bounded scheduling contract locally, not that the exact Windows travel
workload has recovered or meets cadence.

## A failed regression test that should not have been weakened

V6 bounded stale-heap cleanup but made readiness queries non-mutating. The
consumer erases logical tickets while leaving heap entries, so subsequent
admissions encountered a stale head and stopped. I initially called the failing
forecast/prewarm test obsolete and added a fixture-only refresh. That hid a
real consumer contract: the normal run then took39/69s to load and recorded52
incomplete near observations. Passing the modified test did not validate it.

V7 restores the original test and shares one 256-inspection maintenance budget
across the whole admission pump. A new 35-job test verifies near, forecast,
prewarm and multiple background admissions together, including exhausted-budget
deferral and eventual progress. Windows then recorded zero incomplete near
samples and11.137/4.878s barriers, but still missed performance at99.744%
versus99.8%. This separates the corrected starvation defect from the remaining
timing failure. A test that contradicts a refactor must be checked against the
actual consumer before changing its fixture; the old failure remains evidence.
