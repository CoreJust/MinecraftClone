# Owner decision and next-minor handoff

## Read this before resuming

The owner's latest instruction is plans/context only. Do not implement the
fixes, start another qualification cycle, publish the minor, or start a new
gameplay run from this planning request. A new chat will own implementation.

The owner chose EarlyDev 0.1.1 BasicBlocks (planned minor MC-AI-0480), with the three gameplay defects and
their ordinary-gameplay regression checks as its first batch. Use the existing
BasicBlocks scope in [the product roadmap](../ROADMAP.md). Preserve Windows's
standing untouched/secondary restriction until changed by the owner.

## Failed S7 gameplay acceptance

On 19 October 2026, after about 30 seconds with the published normal Mac
package, the owner reported:

1. The OS cursor remained visible.
2. FPS quickly fell to about 80 during movement. Stationary FPS does not
   establish acceptable gameplay performance.
3. Moving forward produced apparently random visible terrain loading order.

The tested archive SHA-256 was
`54768a9010e0b987bb70f71e8b6f15ac529d0eacded31aa76537cf15a30b220b`,
from promotion `cbfd4a640badbc4b803eac9e393326d62626ac9f`.
The owner directed closing EarlyDev 0.1.0 with these known defects and moving
on. This is an owner-directed scope/closure decision, not a clean gameplay
PASS or proof that any defect was fixed. Formal minor publication has not
been performed. The assistant's earlier clean-completion claim was wrong.

The owner explicitly requested this context be retained: "this MUST NOT
REPEAT. Such issues must be easily caught."

## Why passing automation was insufficient

The automated proof included scripted movement; it was not simply a
stationary test. Its display-timing result was accepted despite failed
CPU-loop/request-cadence diagnostics. That did not establish smooth fresh
frames in the ordinary interactive workload. Do not explain the observed
80 FPS as host contention, driver behavior or user setup without reproduction.

Terrain checkpoint completeness and queue assertions did not establish
acceptable visible loading order throughout ordinary travel. Trace generation,
delivery, residency, meshing and publication separately before identifying its
runtime cause. Existing tests passing does not invalidate the user's failure.

Cursor configuration exists in `PlayerClient::beginContinuousLook`, but an
assignment of GLFW cursor mode does not prove correct visible behavior across
normal startup, focus loss and refocus. No accepted exact-package evidence
established that behavior.

These are established QA-design and coordinator-judgment failures. The three
underlying runtime causes remain undiagnosed. More hashes, analyzer passes or
endpoint checks do not substitute for representative product behavior.

## First batch for the new chat

- MC-AI-0477: reproduce and fix cursor capture through the shipped launcher;
  check focus/refocus, relative look and OS cursor restoration on exit.
- MC-AI-0478: reproduce moving FPS and reconcile HUD, fresh rendered frames,
  CPU/simulation and presentation. Measure sustained ordinary movement, turns,
  newly streamed terrain, border traversal and x200 at the agreed settings.
  Neither stationary FPS nor display cadence alone establishes fresh gameplay.
- MC-AI-0479: reproduce apparently random visible loading; test near-first
  service under continuous arrivals, turns, diagonal travel and reversal.
  Inspect the full viewport in motion, not only endpoint recovery or225center
  tiles. Keep position-driven residency and projection-derived LOD intact.

Write failing regressions against the actual production paths before fixes.
Use deterministic subsystem tests for diagnosis and bounded ordinary end-to-end
and visual checks for the user-visible result. Preserve raw failures and
conflicting metrics. Do the obvious gameplay checks before expensive matrices.
Any scripted proxy needs evidence that normal settings/input/scheduling match.

Exact-build owner acceptance or a clearly scoped owner exception remains
separate from automated qualification, launch success, silence, earlier builds
and requests to stop spending time. Relevant gameplay changes invalidate the
affected acceptance. Do not claim these prevention checks already exist.

## Reporting must start automatically with implementation

The new chat must read [RELEASE_REPORTS.md](RELEASE_REPORTS.md) and use
`script/ai_release_reports.py`; this is a standing project rule, not optional
context supplied only by the previous chat. Initialize the next version's
managed folder once its implementation starts. This planning request starts
neither implementation nor a reporting automation.

Every three hours in Europe/Belgrade, deliver the short user-visible report
and validated PDF. Maintain contiguous fixed periods, ordered feature IDs,
all ten sections/line limits, and an activity time-share diagram with honest
measured/estimated/unaccounted basis and no parallel double-counting. Every
four periods, validate/read back the twelve-hour aggregate before pruning its
source PDFs. Retain every twelve-hour PDF and at most three three-hour PDFs.
Remove only the exact managed folder after verified full delivery. No stage
may exceed30minutes; missed deadlines are never silently reset.

Use a version-specific prefix, not the old S7 prefix. Do not recreate the retired
S7 automation. Establish the implementation-start anchor in the new chat and
create its corresponding three-hour heartbeat through the supported automation
tool. A project rule instructs the chat to do this; no scheduler is claimed
to exist until its creation and readback actually succeed.
