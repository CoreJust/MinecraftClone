Run [src/main.cpp](../../src/main.cpp) with `--server [--port PORT] [--render-distance CHUNKS]`, `--player-client`/`--bot-client` (use `--address IP:PORT`). No args/`--local-game` owns/shuts down server/window. Local play uses ephemeral loopback; explicit ports retain semantics. T0 precedes preparation. `--player-client-2d --address IP:PORT --replay stationary|ordinary-turning --seconds 1..120 --evidence DIR` records logical CPU column-map frames; graphics-free `mc_column_map` omits selector.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) sets 100-ms ticks, normalized XYZ, 10,000-subcell remainders, four-bit palette; `World` owns lifecycle/replication. Flat: 32×32. Flight wraps XY, bounds Z, spawns aloft. Entity Z 0–65,535; terrain 0–1,023. Empty high-flight coverage is Ready without chunks. Flight/bypass select terrain/body collision; walking adds gravity/jumps; takeoff preserves impulse. Footprints: Flat 2×2, Flight 10/16; spawns enforce separation.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines bounded CoreLang `@version(0.1.5)` plans. Hosts validate combined backend-comparison program/resource budgets prepublication; rejection starts no runner/world mutation. `flat3d-v1` stores position/yaw/pitch/roll (Z=0; yaw 0 faces +Y, positive +X). Evidence separates replay IDs, inputs, 100-ms ticks and presentation cadence; vertical camera defaults to zero ([scripting guide](../scripting/README.md)).

## Permissions

Bypass requires flight ([POLICY.md](POLICY.md)). [`BlockInteraction.hpp`](../../src/shared/include/shared/world/BlockInteraction.hpp) defines preset/soft/hard effective rights and interval-counted break timing. Read-only wrapped DDA: 5-block eye reach, tied-axis steps, Unknown/Preview stops, renderer-consistent camera/curvature.

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines Protocol 21:
server-authoritative action intents/results, revisioned deltas, sparse edit
snapshots and complete edited-column manifests. Sessions carry a server nonce;
terrain deliveries also validate coverage epoch and delivery token. Profiles
certify generated base columns; explicit edits require every manifested Z-chunk
revision. Dense delivery retains separate credits/receipts. Bulk/control use
channels 1/0; peer/class pending caps are 49,152/16,384 bytes including old-epoch
debt. Coverage advertises actual 640/visual 320; dense collision/start caches use
48/80. Inputs carry XYZ/heading/sequence; positions carry subcells/palette/ACK.
Capabilities 0/1/3 are valid; invalid input and stale ACKs cannot mutate state.

## Server authority

[GameServer](../../src/server/GameServer.cpp) defaults to port `20040`. Polls drain
≤256 events/1 ms; active/idle waits are 1/5 ms. Ticks serialize one action/player;
idle gravity advances revisions without inventing ACKs. Invalid joins,
duplicate characters and pre-join inputs fail; disconnects remove players.

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

Prepared joins retain original T0. Dense 48 permits prompt/start; first public
full Ready requires all 768 startup-reserve profiles. Status
`startup_prefetch_primed_at` records priming; afterward readiness and counters track
current 640 only. Begin/restart/cancel and complete profile clear rearm priming.
Partial reserve loss preserves complete current 640 coverage.
Session/epoch and identity validate before publication. Dense 48/80 delivery
and ACK-gated lead promotion stay separate. Earlier 40/72 was an assistant unit
error. [Generation](GENERATION.md#bounded-materialized-prefetch) documents capacity.

Preview meshes grow from the near-camera connected component and publish after a
cardinal neighbor is ready. Interest changes and bridge removals rebuild it; detached renderer
meshes retire while canonical resident data remains available for remeshing.

Loading shows stage data with a stable elevation palette and centered outlined
Ready prompt. Fresh input starts after its fence; held input stays consumed until
release. Gameplay waits for server ACK; focus loss releases capture. Map pixels
require validated metadata and authoritative highest-solid coverage; otherwise Unknown.

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

[PlayerClient](../../src/client/PlayerClient.cpp) uses GLFW/Vulkan. Cursor controls
yaw/pitch; WASD is camera-relative. F5 cycles first/rear/front without changing input/interest;
first-person hides the body. HUD shows view/F5/5 (macOS may reserve F5/F6).
Camera is distance-bounded and collision-clipped; R reloads renderer.

## Canonical chunks and mesh

`Chunk` validates materials/identity. `SparseWorld` remote reads preserve Unknown
without generation. Base profiles describe solid layered columns; edit manifests
fence missing affected Z sections. Collision/support queries share authoritative
columns and retained edits. Dense-cache churn leaves unedited tile identity
stable; edits/deletion/profile replacement invalidate it.

Changed voxels advance cell/chunk versions. Receipt-gated deltas and atomic
snapshots retain Air/base-reset overrides across eviction and base arrival;
stale updates fail and bounded resync repairs gaps. The 65,536-unit cap counts
stored cells, minimum one/chunk; admission is atomic. `ChunkMesher` suppresses
seams. Terrain LOD composes effective edited chunks with base profiles and
invalidates captures on local/boundary-neighbor revisions.
