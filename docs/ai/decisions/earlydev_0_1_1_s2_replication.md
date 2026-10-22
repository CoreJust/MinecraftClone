# S2 authoritative edit replication

This proposes S2’s edit-replication boundary; the message types and replacement behavior described here do not yet exist. The main [S2 design index](earlydev_0_1_1_s2.md) owns accepted-versus-proposed status and source mapping. Related notes: [world state](earlydev_0_1_1_s2_world.md), [interaction](earlydev_0_1_1_s2_interaction.md), [rendering](earlydev_0_1_1_s2_rendering.md), and [verification](earlydev_0_1_1_s2_verification.md).

## Existing transport boundary

The protocol supports join, sequenced movement, terrain profiles, and full chunk delivery, but no block-action request or live edit delta (`src/shared/include/shared/net/Message.hpp:107-205,274-293`). Chunk payloads carry coordinates, content hashes, and material data, not edit revisions. Session, coverage epoch, and delivery token guard current sends (`Message.hpp:141-168`; `src/server/GameServer.cpp:2377-2480,2525-2595`).

The client skips an already-resident chunk; for a pending same-coordinate chunk, it accepts only a matching hash (`src/client/GameClient.cpp:1247-1307,1391-1403`). Thus initial delivery cannot update peers already holding edited data. S2 needs live per-chunk deltas plus atomic revision-aware chunk replacement or resynchronization. Profile batches are compact surface data, not arbitrary voxel edits.

## Proposed authority and ordering

Send typed action intent with sequenced movement input: action sequence, epoch/base, bounded 3D aim/view, selected material and target hint. Reconstruct the camera ray from accepted pose/view; a hint mismatch rejects. Reject malformed, stale, wrong-session/base, Preview or Unknown actions. Repeated sequences return a retained result or an expired-sequence rejection; they cannot mutate, advance breaking or renew a hold lease. Apply the [interaction rules](earlydev_0_1_1_s2_interaction.md). Missing data uses bounded existing interest loading, never synchronous ray generation; retry requires fresh input.

The server serializes accepted actions with movement and chunk publication. A changed action updates the canonical overlay and advances the target chunk’s edit revision before broadcasting. Typed rejection leaves world and peer state unchanged; clients do not optimistically place or remove blocks. A world-wide edit sequence may label events, but cannot detect gaps because interest filtering skips unrelated chunks. Use each target chunk’s `from_revision` and `to_revision` for continuity.

Each delta identifies epoch/base, target chunk, normalized cell, resulting material and cell version, plus its contiguous chunk revision transition. Check active session and base identity. Ignore duplicate or stale transitions; apply only at the installed `from_revision`. Hold bounded future deltas while requesting a current snapshot. Revisions distinguish ABA changes even when material values repeat. Action results/progress carry action, target/version and server-tick identity for correction and cosmetic-event deduplication.

A full snapshot carries matching epoch/base identity, complete effective voxel state (including Air removals and base-equal writes), and a snapshot watermark equal to its chunk edit revision. Install atomically and only if it is not older than already installed state. Bound newer deltas held during installation, then replay contiguous transitions. Drop prior-epoch traffic. The server also validates worker identity and reapplies the current overlay before publishing generated chunks, so base-only results cannot replace edit state.

## Edited-column coverage

A surface profile summarizes top height/material; it cannot represent holes, tunnels, or overhangs. S1 profiles span the terrain’s Z range, but a base-only profile cannot declare an edited XY tile fully authoritative. Bind a complete manifest of overlay-bearing vertical chunk coordinates and required revisions to the base identity and a manifest watermark. Each XY column contains at most 64 vertical chunks because valid Z spans 1,024 blocks and chunks are 16 blocks high.

When an edit first adds an override in a previously unedited Z chunk, update the manifest and its revision requirement as an ordered world-state change. Install manifests atomically. A client advertises complete edited-column coverage only after every listed chunk is present at its required revision and all intervening deltas are applied. Until then, retain Unknown or partial coverage as appropriate. A base profile may help presentation, but cannot answer authoritative voxel queries for that edited column. Existing `Unknown` behavior is defined by `SparseWorld::queryBlock` (`src/shared/world/SparseWorld.cpp:309-349`).

Interest filtering may omit unrelated global edit IDs, never a promised per-chunk transition. On interest entry, rejoin, or a detected gap, provide current snapshots and the matching manifest rather than unbounded history. The running server preserves edits across reconnect; clients may discard stale nonresident edit caches and rebuild from current state. Server-restart persistence is outside S2; see the [world-state boundary](earlydev_0_1_1_s2_world.md) and 0.1.2 scope.

Use existing transport envelopes and encoded-byte/credit accounting. Bound pending action results, future deltas, chunk resync, and manifest waits within a connection-wide budget. Reserve progress for controls and edits without starving terrain, and preserve terrain progress under edit traffic. If the selected bounds prevent queueing or retaining state, return a typed rejection or resync condition; never silently discard authoritative edits. Define bounds during implementation from the existing envelope contract; do not invent packet byte constants here.

## Acceptance anchors

Verify that (1) changing a client target hint cannot choose the authoritative target; (2) duplicate actions mutate once and typed rejection leaves peers unchanged; (3) live edits reach peers already holding a chunk; (4) per-chunk gaps trigger recovery while skipped global IDs do not; (5) duplicate, stale, wrong-base, ABA, and late snapshots cannot roll state backward; (6) edited-column readiness waits for its full manifest and required Z-chunk revisions, including first edits in new chunks; (7) reconnect and interest re-entry receive current edits and reject old-session traffic; and (8) bounded queues preserve terrain and edit progress under pressure. These are future acceptance cases, not qualification results.
