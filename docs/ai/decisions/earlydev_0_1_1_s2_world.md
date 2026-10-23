# S2 world edits and terrain identity

This is a proposed world-state boundary for S2, not implemented behavior or owner-approved code. The main [S2 design index](earlydev_0_1_1_s2.md) distinguishes accepted requirements from proposals and owns cross-note source mapping. See [interaction](earlydev_0_1_1_s2_interaction.md), [rendering](earlydev_0_1_1_s2_rendering.md), [replication](earlydev_0_1_1_s2_replication.md), and [verification](earlydev_0_1_1_s2_verification.md).

## Current contract and gap

The ordinary path is procedural `TerrainWorld` profiles, a materialized `Chunk`, then server-owned `SparseWorld`. `GameServer` uses this world for collision and publishes accepted generation results server-side (`src/shared/world/TerrainWorld.cpp:1070-1144`; `src/server/GameServer.cpp:743-793,4592-4678`). A chunk is 16³; its local mutation changes content hash and revision only for a changed material (`src/shared/include/shared/world/Chunk.hpp:47-64`; `src/shared/world/Chunk.cpp:99-129`). That revision identifies a materialized chunk, not durable edit state.

`SparseWorld::setBlock` changes a resident chunk. If an authoritative surface exists, it marks the edited chunk as a dense override and pins it against eviction, but the marker does not hold voxel values independently. An edit made before that surface exists may be evicted and regenerated from the base. The default resident cap is 128 chunks, so pinning all edited chunks cannot scale (`src/shared/world/SparseWorld.cpp:108-115,770-793`; eviction: `167-213`). Dense chunks and compact surface profiles are separate; profiles summarize column height and surface material, not arbitrary voxel cuts (`src/shared/include/shared/world/SparseWorld.hpp:123-160,307-315`).

Use `queryBlock` for edit probes: it is non-materializing and reports Materialized, Preview, or Unknown. The non-const `blockAt` may materialize, so it must not be used by a ray (`src/shared/world/SparseWorld.cpp:292-349`). Preview and Unknown are unavailable for edits; reject them without generating. Missing data is not Air.

## Proposed world ownership

Keep procedural terrain immutable as the base. Add one sparse override map owned by the running server world, keyed by normalized chunk and local cell. Each entry stores the cell’s current resulting material and last-write version. Air is an explicit removal. A write equal to the base also remains the latest state, so later base rematerialization cannot undo it. Keep current cell state rather than unbounded history.

Resolve every voxel from the base, then the override when present. Apply that same overlay while publishing generated chunks and forming exact queries, collision reads, column summaries, and meshing inputs. Physics and rendering then see one committed value. Profiles can refresh their overlay-aware top-column summaries but cannot represent arbitrary holes or overhangs; near rendering needs full voxel data. The replication note defines when clients may claim an edited column is completely covered.

Treat dense chunks and profiles as evictable caches. Never pin edited chunks to preserve edits, and never evict authoritative overrides to make cache room. If required authoritative edit state cannot be retained, reject before mutation and leave revisions and peers unchanged. S2 covers the in-memory running server world; save/load is assigned to 0.1.2. Reconnecting to the same running world preserves its overlay, but recovery after server restart is not promised here.

Normalize coordinates before chunk or neighbor lookup. X and Y wrap over 65,536 blocks; Z is valid only from 0 through 1,023; chunks are 16 blocks wide (`src/shared/include/shared/world/SparseWorld.hpp:32-94`). Keep distinct versions for world epoch plus immutable base/model identity, each cell’s last write, and each chunk’s edit revision. A real mutation advances cell and chunk versions; a same-material no-op advances neither. Keep the existing `(revision, content_hash)` as chunk-cache identity, not persistent edit revision.

## Publication and invalidation

The server receive/tick owner serializes edits with chunk publication. Workers produce base terrain only. On result arrival, reject any result with a stale epoch or base identity; otherwise apply the current overlay before publication. A delayed base result therefore cannot overwrite an accepted edit, and worker threads do not mutate canonical edit state.

Invalidate the edited chunk’s exact mesh input, overlay-aware profile/source identity, and any neighbor face touched at a chunk boundary. Normalize neighbors after wrapping so an X/Y seam edit invalidates its opposite-edge neighbor. Existing meshing snapshots a chunk and six neighbor face planes, giving the relevant boundary (`src/shared/include/shared/world/ChunkMesher.hpp:37-49,123-145`; `src/shared/world/ChunkMesher.cpp:119-180`). Far LOD captures must share base/source identity and replace stale results atomically. Edit preservation does not change frustum residency or load policy.

Terrain-model replacement belongs in S2, but active research remains a separate, unqualified input. Its adoption handoff must identify the owner-selected immutable model/config/topology, material and volume APIs, deterministic references, load/stream/FPS/native evidence, affected production stages, and behavior with edited worlds. Preserve X/Y wrapping. Do not select a candidate here. A production switch starts a new world epoch; never reinterpret live edits against another base. S1R6 is an existing consumer and fixture, not proof the model gate passed or a permanent fallback requirement.

## Acceptance anchors

Verify that (1) Air removals and writes equal to base survive cache eviction and reapply on rematerialization; (2) physics, summaries, and near meshes agree on one overlay value; (3) Unknown, Preview, and invalid Z reject without generation or version change; (4) same-material writes are no-ops and stale world identities are rejected; (5) face edits invalidate both chunks, including wrapped seams; (6) resource pressure rejects before mutation and never drops existing overrides; and (7) model changes require a new epoch and overlay-adoption evidence. These are future checks, not measured results.
