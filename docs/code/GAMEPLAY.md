# Gameplay and networking

Run [src/main.cpp](../../src/main.cpp) with `--server [--port PORT] [--render-distance CHUNKS]`,
`--player-client`/`--bot-client`; clients pass `--address IP:PORT`. No args/
`--local-game` owns server/window and shuts both down. Default local play uses an
ephemeral loopback port; explicit server/port options retain their semantics.
T0 precedes preparation.
`--player-client-2d --address IP:PORT --replay stationary|ordinary-turning
--seconds 1..120 --evidence DIR` records logical CPU column-map frames;
graphics-free `mc_column_map` omits the mode selector.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) sets 100 ms ticks,
normalized XYZ, 10,000-subcell remainders and a four-bit palette; `World` owns
lifecycle/replication. Flat is 32×32; Flight wraps XY, bounds Z and spawns aloft.
Entity Z is 0–65,535; terrain is 0–1,023. Empty high-flight coverage is Ready
and generates no chunks.
Flight/bypass select terrain/body collision; walking adds gravity/jumps. Takeoff
preserves impulse. Flat/Flight footprints are 2×2 and 10/16; spawns enforce separation.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines
bounded CoreLang `@version(0.1.5)` plans. Hosts validate combined backend-comparison program/resource budgets before
publication; rejection starts no runner or world mutation. `flat3d-v1` stores
position/yaw/pitch/roll (Z=0; yaw 0 faces +Y, positive yaw +X). Evidence
separates replay IDs, inputs, 100 ms ticks and presentation cadence; vertical
camera input defaults to zero ([scripting guide](../scripting/README.md)).

## Permissions

Bypass requires flight ([POLICY.md](POLICY.md)).

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines versioned
little-endian messages. Protocol20 carries authoritative-profile batches
and acknowledgements bound to session, coverage epoch and delivery token;
previews are not required. Each XY profile certifies all terrain Z. Dense
materialized delivery retains separate credits/receipts. Bulk uses channel 1,
control 0; peer/class pending caps are 49,152/16,384 bytes, including old-epoch debt. Coverage advertises
actual 640 and visual 320 radii; collision/start cache uses dense 48, prefetched 80.
Inputs carry XYZ direction/heading/sequence; positions carry coordinates,
subcells, palette/ACK. Only capability bytes 0/1/3 are valid; stale ACK state
cannot mutate prediction. Invalid input fails before mutation.

## Server authority

[GameServer](../../src/server/GameServer.cpp) defaults to port `20040`.
Cancellation-aware polls drain ≤256 events/1 ms; active/idle waits are 1/5 ms;
ticks share one action permit/player with neutral physics; idle gravity advances
revision without inventing input ACKs. Invalid joins, duplicate characters and pre-join
inputs fail; positions carry revisions/ACKs; disconnects broadcast removals.

Preview batches carry ≤64 operations/10,118 bytes. Eight credits share 34,356 bytes below
ENet's 65,536-byte window; ready/dispatched tiles cap at 128. Receipts flush
before rendering; ACKs refill restored keys. Mixed batches reserve ≤half for
removals; additions use free plus released residency; a lone queue may use full
capacity. Priority resets on interest/heading/ACK changes; popped keys recheck it.

Preview near coverage is a wrapped 225-tile square prioritized before far work.
Preview cancellation excludes authority job IDs. Authority delivery inspects a
bounded connected frontier: missing/disconnected entries rotate, unknown work
is fairly revisited. Session/epoch validation, rejected-key retry, replay-safe
ACK credits and inflight ownership remain separate from queue order.

## Terrain-v2 preparation and authority

[GENERATION.md](GENERATION.md) documents the atlas and regional sampler.
`WorldPreparation` publishes an immutable configured `TerrainWorld` with stage
snapshots. `HeightTileWorkerPool` shares it across previews/chunks, bounded to 128
queued/submitted jobs. Cancellation/drain preserve result ownership; `GameServer`
owns scheduling, stale-result rejection and publication.

Prepared joins retain original T0. Dense 48 permits prompt/start; first public
full Ready additionally requires every exact 768 startup-reserve profile. Status
`startup_prefetch_primed_at` records priming; afterward readiness and counters track
current 640 only. Begin/restart/cancel and complete profile clear rearm priming.
Partial reserve loss preserves complete current 640 coverage.
Session/epoch and identity validate before publication. Dense 48/80 delivery
and ACK-gated lead promotion stay separate. Earlier 40/72 was an assistant unit
error. [Generation](GENERATION.md#bounded-materialized-prefetch) documents capacity.

Preview meshes grow from the near-camera connected component and publish after a
cardinal neighbor is ready. Interest changes and bridge removals rebuild it; detached renderer
meshes retire while canonical resident data remains available for remeshing.

Loading shows stage data with a stable elevation palette. A large, outlined
ready heading/instruction is centered in the window. Fresh key/mouse input starts
after a fence-confirmed prompt; held input stays consumed until release. Gameplay
waits for server ACK; focus loss releases capture. Map pixels need validated
metadata and validated authoritative highest-solid coverage, otherwise Unknown.

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
yaw/pitch; WASD is camera-relative. Prediction replays pending input without
changing authority. F5 cycles first/rear/front without changing input/interest;
first-person hides the body. HUD shows view/F5/5 (macOS may reserve F5/F6).
Camera is distance-bounded and collision-clipped; R reloads renderer.

## Canonical chunks and mesh

`Chunk` validates material IDs and terrain identity. `SparseWorld` remote reads
preserve Unknown without creating generators; exact profiles represent all Z.
Gravity support queries the same authoritative columns as body collision,
including dense edits, and retains real terrain steps. Canonical tile identity
ignores unedited dense-cache admission/eviction but changes for edits, deletion
and profile replacement. `ChunkMesher` suppresses neighbor seams; caches require
exact blocks, neighbors and authoritative identity.
