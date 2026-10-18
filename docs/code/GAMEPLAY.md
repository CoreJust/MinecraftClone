# Gameplay and networking

Run [src/main.cpp](../../src/main.cpp) with `--server [--port PORT] [--render-distance CHUNKS]`,
`--player-client`, or `--bot-client`; clients use `--address IP:PORT`.
No arguments launch a localhost player.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) defines 100 ms
`TICK`, normalized XYZ `Direction`, 10,000-subcell remainders and a four-bit
palette; `World` owns lifecycle and replication. Flat is 32×32; Flight
wraps XYZ and spawns aloft. Independent flight/collision-bypass capabilities
govern swept terrain/body collision and sliding. Walking adds support, gravity
and jumps; elapsed gravity matches replicated velocity; takeoff preserves
impulse and remainders persist. Footprints: Flat 2×2, Flight 10/16; Flat
origins 0–30. Spawn/movement enforce separation.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines
bounded CoreLang `@version("0.1.2")` plans. The host validates source/limits
before publishing; rejection starts no runner and mutates no world. See the
[scripting guide](../scripting/README.md). `flat3d-v1` records
position/yaw/pitch/roll (Z=0); yaw-zero faces +Y, positive yaw turns +X.
Evidence distinguishes replay IDs, inputs, 100 ms ticks and presentation
cadence; vertical camera input defaults to zero.

## Permissions

Collision bypass requires flight ([POLICY.md](POLICY.md)).

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines versioned,
little-endian join/input/position/removal/height-tile messages. Inputs carry
normalized XYZ direction, horizontal heading and sequence; positions carry
authoritative coordinates, subcell remainders, palette and ACK. Palette color is
a stable character hash. Protocol 14 lossless height deltas use a minimum and
packed unsigned offsets; bounded receipts report absent/evicted
tiles after removal-first application; servers refill. Descriptors map radius
to disk counts; invalid inputs fail before mutation.

## Server authority

[GameServer](../../src/server/GameServer.cpp) defaults to localhost `20040`.
Polls drain ≤256 events or 1 ms with cancellation checks; active/idle waits are
1/5 ms, and ticks consume one input/player. Invalid joins, duplicate characters
and pre-join inputs fail; positions carry revisions/ACKs, and disconnects
broadcast removals.

Round-robin packets carry ≤64 operations and ≤8,589 bytes. Eight credits share
a 34,356-byte reserve below ENet's 65,536-byte window; ready/dispatched tiles
cap at 128. Applied receipts flush before rendering. ACKs refill restored
keys. Mixed batches reserve ≤half for removals; additions use free plus released
residency, while a lone queue may use full capacity. Removal priority resets on
interest/heading/ACK changes; popped keys recheck interest.
Near coverage is the wrapped Chebyshev-radius-7 square (225 tiles). Missing
keys precede far work: untracked near keys cancel distant queued work, discard
far-ready payloads and queue near first. Background resumes only when all near
keys are resident, queued, dispatched, ready or in-flight. Delivery also blocks
background additions while a current-near key is neither resident nor in-flight.
World forecasts next-tick coverage from direction, acceleration, subcells and
wrapping. Server/client prioritize that square within the desired disk,
otherwise radial work. Continuation clears one-shot mode.
Client mesh admission uses current ranked candidates while bounded refresh
continues; it never waits for the whole far backlog. Priority changes preserve
the fair unranked cursor. Optional `on_preview_metrics`
aggregates worker, loop/tick/pump/sleep, input and credit timing; S7 logs each
second and disables per-job/loop timing when off.

## Client roles and lifecycle

[GameClient](../../src/client/GameClient.cpp) connects/joins/polls/renders.
A retained peer lasts 30 seconds; 25 ms polls retry every 250 ms until cancelled.
`FrameScheduler` sends input every 100 ms. Successful presentations do not
idle-sleep; failed/absent ones back off at most 1 ms. Positions update world;
presentation snaps/interpolates one `TICK`, holding across gaps. Disconnect
resets/retries.

Timing hooks cover loops/input/ticks. Full-radius x200 preserves normal
scheduling; 225-near coverage alone is insufficient. Inspect current-radius
data/mesh bands during flight, not just endpoint refill
([qualification](../ai/GAMEPLAY_QUALIFICATION.md)). Diagnostic warmup defaults
to 180s (`--full-radius-warmup-seconds`: 1–1,800).

[PlayerClient](../../src/client/PlayerClient.cpp) provides GLFW/Vulkan and HUD.
Cursor controls yaw/pitch; WASD is camera-relative. Prediction replays
unacknowledged inputs without changing authority. F5 cycles first/rear/front
without changing input/interest; first-person hides the body. HUD shows view/F5/5
(macOS may reserve F5/F6). Camera distance is bounded/collision-clipped; R
reloads the renderer.


## S6/S7 moving terrain

S7 radius 1–256 (default 256) covers 51,433 tiles at 128 and 205,861 at 256.
Player position, not camera, sets membership. The protected 15×15 near square
comes first; server far queues use wrapped distance, with heading only breaking
ties. Discovery is bounded to 64 results/pump. Background refill targets 32,768
tiles/s in 128-tile bursts after 250ms settled. CoreLang supplies parameters;
C++ computes heights/stone. Six 128-block layers; spawn uses wave one. HUD
speedups are 2x–500x.

LOD selects by wrapped player distance, vertical FOV and framebuffer extent;
replacement faces stay ≤2 px², with exact terrain/collision. Per-detail
certificates bound candidate tops/walls and installed neighbor TOP profiles;
raw-height bounds enclose distance. Four-by-four buckets accelerate selection
without assuming monotone face areas. Changes recertify neighbors; failed
uploads retain inputs. Jobs recheck raw revisions, neighbor detail and
certification. Profile-only changes reuse geometry only while safe; movement,
projection or profile changes refine. Play/capture share 256 priority inspections,
128 results/call and 2 ms drains. Near/forecast 225-key probes include visible rebuilds;
missing current meshes precede ordinary replacements across ticket refresh.
Stale tickets cannot revive. Intake/removal caps are 512 with deadline checks
every 64 keys. Interest guards retain restored keys; capture adds coverage
barriers, not separate intake.

## S5 chunk data

`Chunk` validates Air/Stone IDs/identity; CoreLang terrain feeds bounded
`SparseWorld`.

## S7 staged generation

`WorldGenerationCoordinator` runs bounded seeded stages; refinements inherit
samples. Final terrain uses exact current-revision collision data; failures
require bounded retries.

## S5 exposed-face mesh

`ChunkMesher` emits exposed faces with neighbor-plane seam suppression; cache
validity requires exact blocks/neighbors/revision, not hashes alone.
