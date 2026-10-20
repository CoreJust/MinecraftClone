# Planned project workflow — 4h/12h iterations and validation sizes

Owner requirements for **MinecraftClone and CoreProject2026**, recorded on
19 October 2026. This is the implementation-chat contract. This planning chat
does not implement tools, start development/reporting, change automations or
publish commits. Tooling adoption runs alongside product development; it must
meet its own reporting/checkpoint deadlines without becoming a blanket feature
prerequisite.

## Iteration planning and review

At snapshot/release development start, divide the plan into major iterations
of at most 12 hours, each with concrete tasks/subtasks and observable outcomes.
When opening a major, split it into three minor iterations of at most 4 hours
with lower-level objectives and acceptance evidence. Do not pre-plan these
minor objectives in the initial snapshot plan. Finish early when ready,
report, then start the next iteration; do not wait to fill a nominal period.

Review results at every minor and major boundary. Report unmet objectives
explicitly, investigate their causes, fix the process/setup and replan the
remaining objectives. Limit investigation/correction to one hour. The next
minor may run in parallel with a minor investigation. A failed major pauses
feature development until its investigation/correction is complete; if that
also exceeds one hour, report the breach and blocker rather than resetting
the clock or silently resuming. Retain original objectives and their outcomes.

Each major must put at least one checked commit on remote `ai-dev`, preferably
more. At the major boundary, all small and medium validation must pass before
the checkpoint commit/push. Record exact source/check identities and remote
readback. Aim for earlier checked checkpoints too; never bypass failing gates
to satisfy a deadline. Report any missed publication requirement honestly.

## Parallel execution

Minimize elapsed delivery time through the actual dependency graph. Start
independent product, defect, dependency and workflow lanes together; overlap
test classification, selector/reporting work and documentation with features.
Use disjoint ownership or isolated worktrees, a shared interface contract and
one integrator for shared files. Integrate checked increments promptly.

Mac and Windows build and validate the same candidate concurrently from the
first runnable increment; do not defer an OS until the other is complete.
Protect benchmark measurements from competing heavy workloads. Schedule
paired-machine tests as shared-resource work, with host roles sequential.
A prerequisite blocks only its actual consumers. Track the critical path and
reassign available capacity to its bottleneck at each review. No lane or
iteration waits merely to fill its nominal duration.

## Validation selection and budgets

Classify tests **and checks**, including sanitizers and static analysis, by
size and affected modules. The future selector must automatically use changed
paths and dependencies, explain its selection and reject unclassified work.

| Lane | Selection | Mac wall-clock maximum |
|---|---|---:|
| Small | All basic small tests and fast code checks | 3 min |
| Medium | All medium tests/checks | 12 min |
| Major boundary | All small + all medium | 15 min |
| Large | Large tests/checks, once for the release candidate | 45 min |
| Full release | Small + medium + large | 60 min |

Routine edits normally require all basic small tests plus affected-module
medium suites. Large work belongs at the release boundary, not each local
edit or iteration. After failure, fix and rerun only failing checks first;
once they pass, resume other pending or affected checks. Reuse a passing
receipt only when its source, configuration and prerequisites still match.
Changed inputs invalidate affected evidence. Required failed or invalidated
large checks may be rerun; do not repeat the whole large matrix unnecessarily.

These are requirements, not measured current compliance. Implementation must
classify the actual inventory, measure the lanes and optimize them to the
budgets without dropping required coverage or merely relabeling slow work.
Include required build/setup work in the reported gate elapsed time. Keep
small/medium/large limits separate as well as enforcing their aggregate.

## Reports and retained evidence

Replace 3h reports with **4h minor reports**. Each **12h major report covers
three lower iterations**. Preserve their actual contiguous start/end times,
including early closure, failure reviews and any parallel investigation; do
not double-count time or hide gaps. Every report states what was achieved in
that precise period, against its objectives, with evidence and next goals.

Sort feature display by **done**, **in progress**, **planned**, retaining a
stable tie order and feature identity. Render these keywords bold and colored
in the PDF. Keep the established report sections and truthful activity chart.
Major reports add product-level results, available screenshots, measurement
tables and summary analysis of key results and problems.

Validate and read back each report/PDF. Validate the three-period aggregate
before pruning only its covered lower reports; retain all major reports and
unconsolidated lower reports. Preserve historical 3h evidence during the
explicit tooling/automation transition. The implementation chat owns actual
state initialization and scheduling after these contracts are supported.
