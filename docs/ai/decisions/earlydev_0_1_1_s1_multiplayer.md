# S1 — Two-laptop multiplayer qualification

The owner requires real Windows/Mac laptop playtests, in addition to
deterministic loopback/subsystem tests. Run a server on one laptop, one client
locally on that host and a second client on the other laptop. Repeat with
**Mac host / Windows remote** and **Windows host / Mac remote**. Record actual
machines, network path, server/client package identities, dependency/backend
pins, seed/configuration, settings and raw logs. An emulator or two local
processes cannot replace the two physical computers.

Use the same Terrain-v2 world identity for both clients. Each client shows
the actual loading map, validates its initial Ready sphere, receives the key
prompt and enters gameplay only after the gate. Distinguish fresh world
creation from late joining an existing world. Compare authoritative content,
top-block pixels at shared coordinates, collision and generation identities;
camera direction or request order must not create a different world.

Predeclare playtests covering simultaneous movement, separated players with
overlapping and disjoint interest, turns/reversals, x20/x30/x200 travel,
wrapping and seeded teleports, late join, disconnect/rejoin and one player
waiting at the start prompt. Inspect models/permissions already supported by
the current game without pulling later-snapshot animation or bot scope into
S1. Test spawn safety and the inherited cursor/moving-frame/loading defects
through normal launchers on the actual packages.

For each role arrangement, collect each player's requested/Ready coverage,
frontier connectivity, server tick cost, delivery/ack latency, backlog/live
bytes and ordinary fresh rendered frames. Exercise the owner performance
scenarios and report each laptop's pass/fail independently, including host
load; do not merge local and remote results into an average. The
[performance contract](earlydev_0_1_1_s1_performance.md) defines the deadlines
and x30 thresholds. Retain failing traces and any scoped owner exception.

The added S1 request supersedes the former Windows-untouched limitation for
this planned qualification. This planning task starts no remote processes,
firewall changes or playtests. Credentials/network setup, if actually needed,
remain an explicit implementation-time prerequisite. Automated passes do not
substitute for exact-package owner playtests or separately authorized release
publication.
