# Gameplay and networking

Run [src/main.cpp](../../src/main.cpp) with `--server [--port PORT] [--render-distance CHUNKS]`,
`--player-client`, or `--bot-client`; clients use `--address IP:PORT`. No
arguments launch a graphical localhost player. Flight joins select free tokens.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) defines the 100 ms
`TICK`, normalized XYZ `Direction`, 10,000-subcell remainders, and four-bit
palette identity. `World` owns spawn, movement, despawn, and replication. Flat
mode retains its 32 by 32 board; Flight wraps XYZ and spawns in clear air.
Movement capabilities independently
enable flight, collision bypass, or both. Flight without bypass sweeps terrain
and player bodies with tangential wall sliding; non-flight uses gravity,
terrain support, jumps, and body collision. Elapsed-time gravity matches jump
displacement to replicated post-tick vertical velocity; takeoff presentation
preserves the impulse. Remainders persist. Player footprints are 2 by 2 cells
in Flat and 10/16 cell in Flight; origins fit cells 0–30 without crossing the
platform edge. Spawn and movement enforce separation.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines
bounded CoreLang `@version("0.1.2")` plans. The host validates source/limits
before publishing; rejected plans start no runner or mutate a world. Grammar is
in the [scripting guide](../scripting/README.md).

Legacy `flat3d-v1` records flat positions and yaw/pitch/roll; Z remains zero.
Camera-relative commands map yaw-zero forward to +Y and positive yaw toward +X
using the cardinal `Direction` payload. Evidence separates replay IDs, input
counts, 100 ms ticks, and presentation cadence.

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
on port `20040` by default. [GameServer.cpp](../../src/server/GameServer.cpp)
drains at most 256 reported events or for 1 ms after each initial poll before
returning to simulation and terrain streaming, checking the run stop request
between polls. Active streams keep a 1 ms network wait; idle servers use 5 ms.
Ticks consume at most one input per player. Invalid joins, duplicate
characters, and unjoined inputs are rejected. Positions carry ACKs and
revisions; disconnects broadcast removals.

After movement, four 16-operation batches rotate among clients.
Four credited batches/peer (34,356 bytes) leave room in ENet's 65,536-byte
shared window; negotiation/throttling may shrink it.
Ready/dispatched tiles cap at 128
(`BenchmarkHooks::on_preview_buffered`). ACKs reconcile keys/refill after restored-interest removals.
The optional `on_preview_metrics` hook collects steady-clock worker-stage,
loop/tick/pump/sleep, input-progress, and credit-turnaround aggregates. S7 stress
tests log at most once per second; without the hook, per-job timing records and
loop metrics stay disabled.

## Client roles and lifecycle

[GameClient.hpp](../../src/client/include/client/GameClient.hpp) and
[GameClient.cpp](../../src/client/GameClient.cpp) connect, join, poll, and
render. Connect retains one peer for 30 seconds; polling is 25 ms with 250 ms
retries and stop cancellation.
`FrameScheduler` sends input every 100 ms with 1 ms maximum sleep. Positions
update world: presentation snaps, then interpolates for one
`TICK`; gaps hold. Disconnect resets and retries.

Benchmark hooks time client loops/scripted input and server ticks; only
`--benchmark-game` skips client idle delay.

[PlayerClient.hpp](../../src/client/include/client/PlayerClient.hpp) and
[PlayerClient.cpp](../../src/client/PlayerClient.cpp) provide the GLFW/Vulkan
client and HUD. Cursor movement controls yaw/pitch; WASD becomes camera-relative
normalized horizontal input. Client prediction replays queued input from
acknowledged state without mutating authoritative `World`. Each
render samples received players into colored 2 by 2 records. F5
cycles first person, rear third person and front-facing third person.
Only display-camera perspective changes; input/interest retain independent look.
First person hides the local body; third-person modes draw it.
HUD shows perspective and F5/5 fallback;
macOS may reserve bare function-row F5/F6, so number-row aliases are reliable.
The pure resolver accepts a bounded unobstructed
distance; collision clips camera presentation without changing authority or
packets. R reloads the renderer on press.

`BotClient` renders nothing and changes direction with probability 1/50 per
input call.

## S6/S7 moving terrain

S7 render distance selects a shared radius 1–256 (default 256): radius 128 has
51,433 tiles; radius 256 has 205,861. Position alone sets membership; heading
prioritizes radius-3/radius-8 circles and a directional ellipse. Server wraps
edge deltas, bounds discovery, and collects at most 64 results per pump.
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
changes. Play reranks 256 pending meshes per call; coverage controls removal.

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
