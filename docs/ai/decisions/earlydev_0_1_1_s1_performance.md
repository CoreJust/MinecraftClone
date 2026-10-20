# S1 — Loading and moving performance contracts

These owner targets supersede the earlier provisional preparation times and
S1's inherited radius-128/99.8%-at-119 Mac acceptance target. The historical
S7 failures remain defects to reproduce and prevent. Python model timings
establish neither end-to-end loading nor native gameplay performance.

| Contract | Mac | Windows | Start / predicate |
|---|---:|---:|---|
| Initial loading | ≤4 s | ≤4 s | World request T0 through first ready-prompt frame; global map, refined safe spawn and Ready 48-block sphere. |
| Basic playable radius | ≤20 s | ≤15 s | Same T0 through complete Ready 320-block sphere; key wait never resets the timer. |
| Flight at x30 | ≥120 fresh FPS for ≥99% of measured moving time | ≥300 fresh FPS for ≥99% of measured moving time | Maintain the complete 320-block sphere around the current player throughout the measured run. |

The owner retained each laptop's existing qualification graphics settings.
Before implementation qualification, freeze the exact machine/OS, package and
dependency revisions, actual framebuffer pixels, FOV, quality/LOD, HUD, power
mode, flight/base speed, renderer/present configuration and display refresh in
the run manifest. Do not lower resolution/quality or replace normal rendering
to pass. Fresh rendered frames and monitor refresh are separate measurements;
report both. An incompatible frame cap must be resolved visibly, not silently
changed or explained away as a pass.

## Timing and FPS accounting

Use optimized normal packages, actual user-visible loading, monotonic raw
timestamps and unique completed-frame IDs. Include world-specific runtime/JIT
or AOT load/compile work, map presentation, scheduling, transfers and initial
materialization after T0. Record warm/cold process/filesystem state and world
cache use; qualify fresh world creation without a prebuilt seed atlas. Keep
joining-existing-world results separate from fresh world creation.

Predeclare a fixed x30 scenario/window before running it. Associate each
successfully completed fresh frame with its wall-clock interval since the
previous fresh frame. The passing-time fraction is the sum of intervals with
duration ≤1/target-FPS divided by total measured moving time, including stalls
and failed/repeated frames in the failing time. Include the final tail; do not
drop outliers or stop the window before a stall. Record interval percentiles,
worst gaps and whole-window throughput alongside the ≥99% predicate.
HUD cadence, CPU loop iterations and repeated presentation cannot substitute.

Check exact Ready sphere membership at every authoritative movement step,
not just an endpoint or center sample. Report coverage deficits and durations,
worker/queue times, upload/publish times, peak live bytes and allocation stalls.
The FPS and radius predicates must pass together on each required laptop.
Begin the sustained full-radius x30 window once that radius is Ready. Also
test pressing the key immediately and moving during the initial fill: the
original T0 deadline must complete around the moving player, not old spawn.
Retain ramp-up frame/coverage data separately; key wait or an undeclared
warm-up cannot hide a missed loading deadline.

## Representative movement and actual speed

The owner requires movement and camera changes that resemble real play:
forward travel, changing direction, and looking ahead, sideways or back while
continuing to move. A fixed-angle cardinal strafe does not satisfy that view
of the workload. Use a deterministic elapsed-time route with changing movement
and look directions, while preserving the existing x30 and coupled sphere/FPS
criteria. Camera clearance should keep the exercised world visible; changing
back-face culling is not part of this request.

The observed `turning-x30-movement-v1` run is invalid as x30 evidence: synthetic
axis values were ±1 rather than keyboard-scale ±127, and the camera traveled
about 91.166 blocks in 89.987 seconds. Workload labels, speedup flags and any
nonzero displacement cannot prove actual speed. The replacement must bind
accepted inputs, authoritative positions and timestamps to the production
movement step, including wrap and acknowledgement sequencing. Capture the
actual camera route as well. Preserve the invalid run and its measurements;
never reuse it as performance acceptance.

## Scheduling and qualification sequence

Prioritize the current connected frontier and prefetch along velocity, with
turn/reversal allowance. Size the extra lead from actual x30 displacement and
measured tail service latency; do not invent a universal blocks/second value.
Reserve work/memory for the mandatory sphere, and throttle speculative outer
work before it can delay nearby results. Uniform Air/solid compression is
permitted only with exact content equivalence, not a fake readiness mask.

Run deterministic CPU correctness first, then the 2D normal-game mode for
generation/streaming cost, then ordinary Vulkan gameplay for these FPS goals.
The cheaper 2D renderer cannot establish 3D FPS. Include stationary spawn,
continuous flight, turns, reversals, wrapping and new terrain. x20/x200 and
teleports are additional stress/replay cases; the owner specified the coupled
radius/FPS threshold at x30. Record their costs rather than silently applying
or weakening that threshold elsewhere.

The new request explicitly includes Windows qualification and two-laptop
multiplayer in S1, superseding the earlier Windows-untouched restriction for
these activities in the later implementation chat. It authorizes no Windows
run in this planning task. Retain applicable Android correctness/platform
gates without inventing Android FPS targets. Exact-package owner playtests
and publication authority remain separate from automated results.
