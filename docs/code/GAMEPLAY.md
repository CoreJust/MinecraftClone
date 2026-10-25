Run [src/main.cpp](../../src/main.cpp) with `--server [--port PORT] [--render-distance CHUNKS]`, `--player-client`/`--bot-client` (use `--address IP:PORT`). No args/`--local-game` owns server/window lifetime; local play uses ephemeral loopback; explicit ports are unchanged. T0 precedes preparation. `--player-client-2d --address IP:PORT --replay stationary|ordinary-turning --seconds 1..120 --evidence DIR` records logical CPU column-map frames; graphics-free `mc_column_map` omits selector.

## Shared simulation

[World](../../src/shared/include/shared/world/World.hpp) sets 100-ms ticks, normalized XYZ, 10,000-subcell remainders, four-bit palette; it owns lifecycle/replication. Flat: 32×32. Flight wraps XY, spawns aloft. Entity Z 0–65,535; terrain 0–1,023. Empty high-flight coverage: Ready without chunks. Flight/bypass select terrain/body collision; walking adds gravity/jumps; takeoff preserves impulse. Footprints: Flat 2×2, Flight 10/16; spawns enforce separation. Shift descent defers terrain only after bounds, other-player and start-height checks; vertical sweep resolves support.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines bounded CoreLang `@version(0.1.5)` plans. Hosts validate combined backend-comparison program/resource budgets prepublication; rejection starts no runner/world mutation. `flat3d-v1` stores position/yaw/pitch/roll (Z=0; yaw 0 faces +Y, positive +X). Evidence separates replay IDs, inputs, ticks and presentation; vertical camera defaults to zero ([scripting guide](../scripting/README.md)).

## Permissions

Bypass requires flight ([POLICY.md](POLICY.md)). [`BlockInteraction.hpp`](../../src/shared/include/shared/world/BlockInteraction.hpp) defines preset/soft/hard effective rights and interval-counted break timing. Read-only wrapped DDA: 5-block eye reach, tied-axis steps, Unknown/Preview stops, renderer-consistent camera/curvature.

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines Protocol 24
for authoritative actions/results, revisioned deltas, sparse edit snapshots and
manifests. Sessions use nonce; deliveries validate epoch/token. Profiles certify
columns; edits require manifested Z-chunk revisions. Dense credits/receipts stay
separate. Bulk/control channels 1/0; caps: peer 49,152, class 16,384 bytes with
old-epoch debt. Coverage actual/visual: 640/320; caches: 48/80. Inputs:
XYZ/heading/sequence; positions: subcells/palette/ACK. IDs 0–6 fixed; Sand 7;
v23 peers reject v24 frames. Height tiles carry V9 source elevation, biome and
coast metadata. Rights masks allow IDs 1–7. Capabilities
0/1/3; invalid input/stale ACKs cannot mutate state.

## Server authority

[GameServer](../../src/server/GameServer.cpp) defaults to port `20040`. Polls drain
≤256 events/1 ms; active/idle waits are 1/5 ms. Ticks serialize one action/player;
idle gravity advances revisions without inventing ACKs. Invalid joins, duplicate characters and pre-join inputs fail; disconnects remove players.

Sequenced intents validate session, accepted eye aim, reach, visibility,
mode/rights and cell version. Hard vetoes win. Breaks lose credit on target,
version/policy changes, release or lease expiry. Placement needs a fresh press
and rejects body overlap. Duplicates cannot recommit. Loopback mode
requests use the local grant; remote requests require `minecraft:mode.change`.

Preview batches retain ≤64 operations/10,118 bytes, 128 ready/dispatched tiles
and eight credits sharing 34,356 bytes below ENet's 65,536-byte window. Removals
reserve capacity; interest/heading/ACK changes reorder work. Missing or
disconnected authority entries rotate fairly. Preview cancellation excludes
authority jobs. Session/epoch checks, replay-safe credits and ownership remain separate.

## Terrain-v2 preparation and authority

[GENERATION.md](GENERATION.md) documents the atlas and regional sampler.
`WorldPreparation` publishes an immutable configured `TerrainWorld` with stage
snapshots. `HeightTileWorkerPool` shares it across previews/chunks, bounded to 128
queued/submitted jobs. Cancellation/drain preserve result ownership; `GameServer`
owns scheduling, stale-result rejection and publication.

Prepared joins keep T0. Dense 48 permits prompt/start; first public full Ready
requires 768 startup-reserve profiles. `startup_prefetch_primed_at` records priming;
readiness then tracks current 640. Begin/restart/cancel or complete-profile clear
rearms it; partial reserve loss preserves current complete coverage. Session/epoch/
identity gate publication. Dense 48/80 delivery and ACK-gated promotion stay
separate. See [Generation](GENERATION.md#bounded-materialized-prefetch).

Prepared Flight joins without an explicit spawn use nearest safe dry land
near world center, with wrapped distance and deterministic ties. Support stays asynchronous; explicit spawns retain validation. See
[nearest-land spawn](GENERATION.md#nearest-land-prepared-spawn).

Preview meshes grow from near-camera connected coverage and publish after a
cardinal neighbor is ready. Interest changes/removals rebuild them; retired meshes
leave canonical resident data available for remeshing.

Preparing/Connecting show a backdrop. Generation pairs truthful status/counters
with a square map; six captions are illustrative. Ready shows final map/prompt;
fresh input starts after its frame. Held keys stay consumed until release; gameplay
waits for ACK; focus loss releases capture. Pixels need matching metadata and
authoritative highest-solid coverage; otherwise Unknown.

## Client and qualification

`GameClient` uses cancellable 25 ms polls, 250 ms retries and 30-second peers.
`FrameScheduler` preserves 100 ms phase and skips missed ticks. Interpolation
holds gaps over one tick; flight is linear in Z, gravity/collision use
velocity-continuous Hermite. Prediction replays pending input against ACKs.

Desktop/Android share `TerrainLodRuntime`: persistent prioritized discovery and
capture queues preserve useful columns across movement; up to eight captures
fit each deadline. Async scene tickets reject stale sources or lost required
rendered columns. Android background rendering returns after platform events.

Hooks bind deadlines, inputs, accepted poses and camera.
[Qualification](../ai/GAMEPLAY_QUALIFICATION.md) separates S1's turning x30 and
full 640 profile route from generic 320 geometry and x200 stress. T0 never resets. Legacy
180-second warmup is non-gating.

[PlayerClient](../../src/client/PlayerClient.cpp) uses GLFW/Vulkan. Cursor aims;
camera-relative WASD. LMB breaks while held; RMB places once; B cycles blocks, G
requests mode.
F5 cycles views without changing input/interest; first-person hides the body.
Camera collision-clips; R reloads.

## Canonical chunks and mesh

`Chunk` validates materials/identity. `SparseWorld` remote reads preserve Unknown
without generation. Base profiles describe solid columns; edit manifests fence missing Z sections. Collision/support share authoritative columns and edits. Dense-cache churn leaves unedited tile identity
stable; edits/deletion/profile replacement invalidate it.

Voxels advance cell/chunk versions. Receipt-gated deltas/snapshots retain
Air/base-reset overrides across eviction/base arrival; stale updates fail and
bounded resync repairs gaps. The atomic 65,536-unit cap counts stored cells
(minimum one/chunk). `ChunkMesher` suppresses seams. Terrain LOD combines edits
with base profiles; local/boundary-neighbor revisions invalidate captures.
