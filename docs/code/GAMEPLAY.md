# Gameplay and networking

Run [src/main.cpp](../../src/main.cpp) with `--server [--port PORT] [--render-distance CHUNKS]`,
`--player-client`, or `--bot-client`; clients use `--address IP:PORT`. No
arguments launch a graphical localhost player. Flight joins select free tokens.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) defines the 100 ms
`TICK`, normalized XYZ `Direction`, 10,000-subcell remainders, and four-bit
palette identity. `World` owns spawn, movement, despawn, and replication. Flat
retains a 32×32 board; Flight wraps XYZ and spawns in clear air. Capabilities
independently enable flight and collision bypass. Flight without bypass sweeps
terrain and player bodies with tangential sliding; walking uses gravity, terrain
support, jumps, and body collision. Elapsed-time gravity matches jump displacement
to post-tick replicated vertical velocity; takeoff preserves its impulse.
Remainders persist. Footprints are 2×2 cells in Flat and 10/16 in Flight; origins
fit 0–30 without crossing platform edges. Spawn and movement enforce separation.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines
bounded CoreLang `@version("0.1.2")` plans. The host validates source and limits
before publishing; rejected plans neither start a runner nor mutate the world.
See the [scripting guide](../scripting/README.md) for grammar.

Legacy `flat3d-v1` records flat positions and yaw/pitch/roll; Z remains zero.
Camera-relative commands map yaw-zero forward to +Y and positive yaw toward +X
using normalized `Direction`. Evidence separates replay IDs, input counts,
100 ms ticks, and presentation cadence. Conversion takes named
`ScenarioCameraDirectionInput` fields; vertical defaults to zero.

## Permissions

Generation-tagged movement permissions are scenario-tested for replication and
velocity; collision bypass requires flight (see [POLICY.md](POLICY.md)).

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines versioned,
little-endian join/input/position/removal and height-tile messages. Inputs carry
normalized three-axis direction, horizontal view heading,
and a sequence; positions carry authoritative coordinates, subcell remainders,
palette identity, and acknowledgement. Palette color is a stable hash of
character identity.

Protocol 12 preserves the descriptor layout. `max_height_tiles` matches a
radius 1–256 disk cardinality; clients derive residency budgets. Invalid counts,
payloads, radii, versions, or seeded-world joins fail before mutation.

## Server authority

[GameServer.hpp](../../src/server/include/server/GameServer.hpp) binds localhost
port `20040` by default. [GameServer.cpp](../../src/server/GameServer.cpp) drains
at most 256 reported events or 1 ms after the initial poll, checking stop requests
between polls before simulation/streaming. Active streams wait 1 ms; idle servers
wait 5 ms. Ticks consume at most one input/player. Invalid joins, duplicate
characters, and unjoined inputs are rejected; positions carry ACKs/revisions;
disconnects broadcast removals.

After movement, four 16-operation batches rotate among clients. Four credited
batches/peer (34,356 bytes) fit ENet's shared 65,536-byte window; negotiation/
throttling may shrink it. Ready/dispatched tiles cap at 128 (`BenchmarkHooks::on_preview_buffered`).
ACKs reconcile keys/refill after restored-interest removals.
Center/heading changes restart near-first admission, reclaiming unstarted distant
jobs only when near coverage is missing. Refills reorder pending queues;
running/completed jobs remain intact.
Optional `on_preview_metrics` collects steady-clock worker-stage,
loop/tick/pump/sleep, input-progress, and credit-turnaround aggregates. S7 stress
logs at most once per second; without it, per-job timings and loop metrics are
disabled.

## Client roles and lifecycle

[GameClient.hpp](../../src/client/include/client/GameClient.hpp) and
[GameClient.cpp](../../src/client/GameClient.cpp) connect, join, poll, and render.
Connect retains one peer for 30 seconds; polling is 25 ms, retrying every 250 ms
with stop cancellation. `FrameScheduler` sends input every 100 ms with at most
1 ms sleep. Positions update world; presentation snaps, then interpolates one
`TICK`, holding across gaps. Disconnect resets and retries.

Benchmark hooks time client loops/scripted input and server ticks; only
`--benchmark-game` skips client idle delay.

[PlayerClient.hpp](../../src/client/include/client/PlayerClient.hpp) and
[PlayerClient.cpp](../../src/client/PlayerClient.cpp) provide the GLFW/Vulkan
client and HUD. Cursor controls yaw/pitch; WASD maps to camera-relative normalized
horizontal input. Prediction replays queued input from acknowledged state without
mutating authoritative `World`; renders sample received players into colored 2×2
records. F5 cycles first-person, rear, and front-facing third-person views; only
display perspective changes, while input/interest keep independent look. First
person hides the local body. HUD shows perspective and F5/5 fallback (macOS may
reserve bare F5/F6). The pure resolver bounds unobstructed camera distance;
collision clips presentation without changing authority or packets. R reloads the
renderer on press.

`BotClient` renders nothing and changes direction with probability 1/50 per
input call.

## S6/S7 moving terrain

S7 radius 1–256 (default 256) has 51,433 tiles at 128; 205,861 at 256.
Position sets membership; heading
prioritizes radius-3/radius-8 circles and a directional ellipse. Server wraps
edge deltas, bounds discovery, and collects at most 64 results per pump.
Full admission preserves refill cursors, including running jobs.
Generation targets 8,192 tiles/s, with 128-tile bursts after 250 ms settled.
CoreLang 0.1.2 supplies wave parameters; C++ computes heights and stone. Six
128-block layers; spawn uses the first wave. HUD speedups: 2x–500x.

LOD uses wrapped player distance, vertical FOV, and framebuffer extent to bound
replacement faces to 2 px²; terrain/collision stay exact. Boundary strips preserve
membership; 4×4 buckets certify detail. Bounds enclose complete raw neighbors and
installed edge profiles, retaining revisions/detail. Accepted base changes
recertify neighbors; failed uploads retain inputs. Revision/projection changes
invalidate certificates. Jobs recheck revisions and target detail before
publication. Meshes refine asynchronously after movement/projection/profile
changes. Play/capture share 256 reranks and 16 jobs per call. A 225-key near
probe tickets missing resident meshes before distant work; erased/requeued
tickets cannot revive stale entries. Coverage controls removal.

## S5 chunk data

`Chunk` stores checked Air/Stone IDs and identity; staged CoreLang terrain feeds
bounded `SparseWorld` residency.

## S7 staged generation

`WorldGenerationCoordinator` runs bounded seeded server stages. Refinements
inherit ancestor samples; final materialization uses exact height-tile terrain
for current-revision collision. Failed stages require explicit bounded retry.

## S5 exposed-face mesh

`ChunkMesher` emits stable exposed faces. Neighbor planes suppress seams; cache
validity requires exact blocks, neighbors, and revision, not a content hash alone.
