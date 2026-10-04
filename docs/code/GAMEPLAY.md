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
bounded CoreLang `@version("0.1.2")` plans. The host validates source and limits
before publishing an immutable plan; rejection cannot mutate a world or start
the runner. Grammar is in the [scripting guide](../scripting/README.md).

Legacy `flat3d-v1` records flat-plane positions and yaw/pitch/roll; Z remains
zero. Camera-relative commands lower yaw-zero forward to +Y and positive yaw
toward +X using the cardinal `Direction` payload. Evidence separates replay ID,
camera-input count, 100 ms server ticks, and presentation cadence.

## Permissions

The server publishes generation-tagged movement permissions. Scenarios verify
replicated capabilities and velocity. Collision bypass requires flight. See
[POLICY.md](POLICY.md).

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines versioned,
little-endian join, input, player-position/removal, and height-tile streaming
messages. Inputs carry normalized three-axis direction, horizontal view heading,
and a sequence; positions carry authoritative coordinates, subcell remainders,
palette identity, and acknowledgement. The server derives one of 16 stable
palette colors from character identity.

Protocol 12 retains the descriptor layout. `max_height_tiles` must match a
radius 1–256 disk cardinality; clients derive radius and residency budgets.
Malformed counts/payloads, conflicting radii, and old/mixed versions fail before
mutation. Seeded-world join mismatches fail before gameplay.

## Server authority

[GameServer.hpp](../../src/server/include/server/GameServer.hpp) creates a
localhost server on configurable port `20040`.
[GameServer.cpp](../../src/server/GameServer.cpp) drains events each 100 ms tick
and consumes at most one queued input per player. Mismatched-world joins,
duplicate characters and unjoined inputs are rejected. Positions carry
acknowledged sequences and monotonic revisions; disconnect broadcasts removal.

After movement, four 16-operation batches rotate among clients.
Four outstanding credited batches/peer (34,356 bytes) leave gameplay headroom
in ENet's shared 65,536-byte default window; negotiation/throttling may shrink it.
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

`GameClientBenchmarkHooks` times loops, scripts input, and skips idle delay only
for `--benchmark-game`. The server hook times ticks.

[PlayerClient.hpp](../../src/client/include/client/PlayerClient.hpp) and
[PlayerClient.cpp](../../src/client/PlayerClient.cpp) provide the GLFW/Vulkan
client and HUD. Cursor movement controls yaw/pitch; WASD becomes camera-relative
normalized horizontal input. Client prediction replays queued local input from
acknowledged server state without mutating the authoritative `World`. Each
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

S7 server/benchmark `--render-distance CHUNKS` selects radius 1–256 (default 256):
128 has 51,433 tiles; 256 has 205,861. Selected-radius orders have shared ownership.
Membership depends solely on player position; heading prioritizes radius-3/radius-8
circles and a directional ellipse to the former S6 band. Background generation
permits 8,192 tiles/s, with 128-tile bursts after 250 ms settled. CoreLang 0.1.2
supplies wave parameters; C++ computes heights/stone. Six terrain layers are
128 blocks apart; spawn uses the first wave. HUD speedups span 2x–500x.

LOD uses wrapped player distance and vertical FOV/framebuffer extent to
bound replacement LOD-cell faces to 2 px². Terrain/collision remain exact. Boundary
strips maintain membership; 4×4 buckets certify detail.
Bounds enclose complete raw neighbors and installed edge profiles, including
retained revisions/detail. Accepted base publication/removal recertifies neighbors;
failed uploads retain inputs. Revision/projection changes invalidate certificates. Jobs
recheck revisions and target detail; publication uses certified
detail directly. Displayed meshes refine asynchronously after movement/projection/profile
changes. Play reranks 256 pending meshes per call; coverage counts control removal.

## S5 chunk data

`Chunk` stores checked Air/Stone IDs and content identity. CoreLang terrain and
staged generation feed bounded `SparseWorld` residency.

## S7 staged generation

`WorldGenerationCoordinator` runs bounded seeded server stages. Refinements
inherit ancestor samples; final materialization uses exact height-tile terrain
for current-revision collision. Failed stages require explicit bounded retry.

## S5 exposed-face mesh

`ChunkMesher` emits stable exposed faces. Neighbor planes suppress seams; cache
validity requires exact blocks, neighbors, and revision, not a content hash alone.
