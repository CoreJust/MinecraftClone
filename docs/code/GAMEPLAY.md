Use [src/main.cpp](../../src/main.cpp) with `--server [--port PORT] [--render-distance CHUNKS]` and `--player-client`/`--bot-client --address IP:PORT`. No args or `--local-game` starts an ephemeral loopback server and window; explicit ports are unchanged, and T0 precedes preparation. `--player-client-2d ... --evidence DIR` records logical CPU column-map frames; graphics-free `mc_column_map` omits the selector.

## Shared simulation

[World](../../src/shared/include/shared/world/World.hpp) sets 100-ms ticks, normalized XYZ, 10,000-subcell remainders, four-bit palette; it owns lifecycle/replication. Flat: 32×32. Flight wraps XY, spawns aloft. Entity Z 0–65,535; terrain 0–1,023. Empty high-flight coverage: Ready without chunks. Flight/bypass select terrain/body collision; walking adds gravity/jumps; takeoff preserves impulse. Footprints: Flat 2×2, Flight 10/16; spawns enforce separation. Shift descent defers terrain only after bounds, other-player and start-height checks; vertical sweep resolves support.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines bounded CoreLang `@version(0.1.5)` plans. Hosts validate combined backend-comparison program/resource budgets prepublication; rejection starts no runner/world mutation. `flat3d-v1` stores position/yaw/pitch/roll (Z=0; yaw 0 faces +Y, positive +X). Evidence separates replay IDs, inputs, ticks and presentation; vertical camera defaults to zero ([scripting guide](../scripting/README.md)).

## Permissions

Bypass requires flight ([POLICY.md](POLICY.md)). [`BlockInteraction.hpp`](../../src/shared/include/shared/world/BlockInteraction.hpp) defines preset/soft/hard effective rights and interval-counted break timing. Read-only wrapped DDA: 6-block eye reach, tied-axis steps, Unknown/Preview stops, renderer-consistent camera/curvature. Material break durations are unchanged.

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines Protocol 24
for authoritative actions/results, revisioned deltas, sparse edit snapshots and
manifests. Sessions use nonce; deliveries validate epoch/token. Profiles certify
columns; edits require manifested Z-chunk revisions. Dense credits/receipts stay separate. Bulk/control channels `1/0`; peer cap
`49,152 B`; old-epoch debt stays charged. Startup canonical/dense class windows:
`16,384 B`. Post-start canonical precedes dense refill and may borrow spare peer
bytes, preserving edit/preview reserves, in-interest columns and authority fences.
Coverage actual/visual: 640/320; caches: 48/80. Inputs: XYZ/heading/sequence;
positions: subcells/palette/ACK. IDs 0–6 fixed; Sand 7; v23 peers reject v24
frames. Height tiles carry V9 source elevation, biome and coast metadata. Rights
masks allow IDs 1–7. Capabilities 0/1/3; invalid input/stale ACKs cannot mutate
state.

## Server authority

[GameServer](../../src/server/GameServer.cpp) defaults to port `20040`. Polls drain
≤256 events/1 ms; active/idle waits are 1/5 ms. Ticks serialize one action/player;
idle gravity advances revisions without inventing ACKs. Invalid joins, duplicate characters and pre-join inputs fail; disconnects remove players.

Sequenced intents check session, accepted aim, reach/visibility, mode/rights,
version; hard vetoes win. Break credit resets on target/version/policy change,
release or lease expiry. Each placement sequence rejects body overlap; duplicates
cannot recommit. Loopback uses local mode grants; remote changes require
`minecraft:mode.change`.

Preview batches cap at 64 operations/12,678 bytes and fit a maximum 16-bit tile. Eight credits share 34,356 bytes within ENet's 65,536-byte window. Removals reserve capacity; interest, heading and ACKs reorder work. Missing entries rotate fairly. Preview cancellation excludes authority jobs; session/epoch and replay-safe credits remain separate.

## Terrain-v2 preparation and authority

[GENERATION.md](GENERATION.md) documents atlas and regional sampling.
`WorldPreparation` publishes an immutable `TerrainWorld` with stage snapshots.
`HeightTileWorkerPool` shares it across previews/chunks, bounded to 128 jobs;
`GameServer` owns scheduling, stale-result rejection and publication. LocalGame
reuses this world for client L0; remote clients prepare against the negotiated
session.

Prepared joins keep T0. Dense 48 permits prompt/start; full Ready requires 768
startup-reserve profiles, then current 640. `startup_prefetch_primed_at` records
priming. Begin/restart/cancel or complete-profile loss rearms it; partial reserve
loss preserves current coverage. Session/epoch/identity fence publication. Dense
48/80 delivery and ACK promotion stay separate. See
[materialized prefetch](GENERATION.md#bounded-materialized-prefetch).

Unconfigured Flight joins choose nearest safe dry land near world center by wrapped
distance and deterministic ties. Support stays asynchronous; explicit spawns are
validated. See [nearest-land spawn](GENERATION.md#nearest-land-prepared-spawn).

Preview meshes publish from connected near-camera coverage after cardinal
neighbors are ready. Interest changes rebuild them; canonical data remains
available for remeshing.

Preparing/Connecting show progress over a map; Ready shows the final map and
prompt. Fresh input starts gameplay after its frame; gameplay waits for server
ACK, and focus loss releases cursor capture. Pixels require matching metadata and
authoritative coverage; otherwise they are Unknown.

## Client and qualification

`GameClient` uses cancellable 25 ms polls, 250 ms retries and 30-second peers.
`FrameScheduler` preserves 100 ms ticks and skips missed ones. Interpolation holds
gaps over a tick; flight is linear in Z, gravity/collision use velocity-continuous
Hermite, and prediction replays pending input after ACKs.

Desktop/Android share `TerrainLodRuntime`: prioritized discovery/capture queues
preserve useful columns across movement, fitting up to eight captures per
deadline. Async tickets reject stale sources or missing required columns. Android
returns from background rendering after platform events.

Hooks bind deadlines, inputs, accepted poses and camera. [Qualification](../ai/GAMEPLAY_QUALIFICATION.md)
separates S1's turning x30/640-profile route from generic 320 geometry and x200
stress. T0 never resets; legacy 180-second warmup is non-gating.

[PlayerClient](../../src/client/PlayerClient.cpp) uses GLFW/Vulkan: cursor aiming,
camera-relative WASD. `LMB` breaks while held; `RMB` places immediately and every
`200 ms` at the current target. Release, focus/map/mode change or session reset
requires a new press. `B` cycles blocks; `G` requests mode; `F5` changes views
without changing input/interest. First-person hides the body; camera
collision-clips; `R` reloads.

## Canonical chunks and mesh

`Chunk` validates materials/identity. `SparseWorld` remote reads preserve Unknown
without generation. Base profiles describe solid columns; edit manifests fence missing Z sections. Collision/support share authoritative columns and edits. Dense-cache churn leaves unedited tile identity
stable; edits/deletion/profile replacement invalidate it.

Voxels advance cell/chunk versions. Receipt-gated deltas/snapshots retain
Air/base-reset overrides across eviction/base arrival; stale updates fail and
bounded resync repairs gaps. The atomic 65,536-unit cap counts stored cells
(minimum one/chunk). `ChunkMesher` suppresses seams. Terrain LOD combines edits
with base profiles; local/boundary-neighbor revisions invalidate captures.
