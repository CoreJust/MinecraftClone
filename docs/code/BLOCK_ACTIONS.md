# Authoritative block actions

`BlockInteraction.hpp` defines preset, soft and hard rights and interval-counted
breaks. Bypass requires flight. Read-only wrapped DDA uses six-block eye reach,
tied-axis steps, Unknown/Preview stops and renderer-consistent camera/curvature.

Movement and block requests use the same ordered reliable game channel. The
server retains FIFO block requests and processes earlier queued movement before
deciding their targets. Session, sequence, aim, reach, visibility, rights,
material, occupancy and cell-version checks remain authoritative.

When an incoming block request finds both queues empty, the server can decide
instant placement, cancellation or server-authorized QuickBreak during
network polling. It uses the existing validator and edit publisher. An authority
tick marker preserves the existing one-decision-per-player-per-tick limit,
including when reception occurs inside the tick's network poll. It does not
skip an earlier queued action or move the player speculatively.

Ordinary timed breaks stay on the 100 ms authority clock. Full intervals,
renewal leases, cancellation, target/version changes and policy resets retain
their existing rules. This path changes neither the simulation tick nor normal
break durations. A queued movement or block request can still defer a later
instant action until the next tick.

Accepted edits publish the revisioned manifest and block delta through the
existing delivery path. Client canonical queries and collision update from
accepted state; mesh replacement and GPU display are separate observations.
An exact mesh patch retains the active immutable terrain root and river cache.

`GameServerActionTest` checks pre-tick instant decisions, pending-movement
ordering, FIFO, duplicate commitment and full timed-break intervals. Native
button input, moving-player latency, GPU completion and owner acceptance require
their own evidence; a CPU renderer stub cannot qualify them.
