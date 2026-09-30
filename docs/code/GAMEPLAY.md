# Gameplay and networking

Run [src/main.cpp](../../src/main.cpp) with `--server [--port PORT]`,
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

`Message.cpp` prepends a magic byte, protocol version, and tag. Version 11
advertises the full radius-256 height-tile capacity. Decoding
requires a known, complete, valid, non-trailing payload and rejects old or
mixed versions before any authority mutation. The join configuration identifies
the selected seeded world and the server rejects mismatches before gameplay.

## Server authority

[GameServer.hpp](../../src/server/include/server/GameServer.hpp) creates a
localhost server on configurable port `20040`.
[GameServer.cpp](../../src/server/GameServer.cpp) drains events each 100 ms tick
and consumes at most one queued input per player. Mismatched-world joins,
duplicate characters and unjoined inputs are rejected. Positions carry
acknowledged sequences and monotonic revisions; disconnect broadcasts removal.

After movement responses, network pumps rotate four 16-operation batches across
clients. Credits bound delivery; streams cap ready plus dispatched tiles at 128,
observed by `BenchmarkHooks::on_preview_buffered`. Acknowledgements
reconcile keys and reset refill after restored-interest removals.

## Client roles and lifecycle

[GameClient.hpp](../../src/client/include/client/GameClient.hpp) and
[GameClient.cpp](../../src/client/GameClient.cpp) connect, join, poll, and
render. `FrameScheduler` sends input every 100 ms and sleeps at most one
millisecond between presentation attempts. Replicated positions update the
local world; the first presentation snaps, later updates interpolate for one
`TICK`, and gaps hold. Disconnect resets the connection and retries.

`GameClientBenchmarkHooks` times connected production loops, supplies scripted
input, and skips idle delay only for `--benchmark-game`. The server's optional
hook times authoritative ticks.

[PlayerClient.hpp](../../src/client/include/client/PlayerClient.hpp) and
[PlayerClient.cpp](../../src/client/PlayerClient.cpp) provide the GLFW/Vulkan
client. Gameplay enables the HUD. GLFW cursor movement controls yaw/pitch;
W/S and A/D become camera-relative normalized horizontal directions.
The shared client predicts only its local player's queued input, then rebuilds
that prediction from acknowledged server state; it never mutates the
authoritative `World`. Each render samples
every received player presentation into colored 2 by 2 render records. F5
cycles first person, rear third person, and front-facing third person; only
the display camera changes, while input and interest headings continue to use
the independent local look camera. First person hides the local body and both
third-person modes draw it. The HUD displays the perspective and F5/5 fallback;
macOS may reserve bare function-row F5/F6, so number-row aliases are reliable.
The pure resolver accepts a bounded unobstructed
distance, so terrain/collision presentation can clip a third-person camera
without changing authority or packets. R reloads the renderer on a press edge
stored per client instance.

`BotClient` renders nothing and changes a persistent random direction with
probability 1/50 per input call.

## S6/S7 moving terrain

S7 replaces S6's radius-45 circle with a wrapped radius-256 disk (205,861 tiles).
Membership depends solely on player position; heading prioritizes radius-3/radius-8
circles and a directional ellipse to the former S6 band. Background generation
permits 8,192 tiles/s, with 128-tile bursts after 250 ms settled. CoreLang 0.1.2
supplies wave parameters; C++ computes heights/stone. Six terrain layers are
128 blocks apart; spawn uses the first wave. HUD speedups span 2x–500x.

Visual LOD uses shortest wrapped player distance, actual vertical FOV and physical
framebuffer extent; simplified source faces conservatively occupy at most 2 px².
Terrain/collision remain exact. Client membership uses boundary strips; cached
elevation bounds in 4×4 tile buckets certify unchanged detail over viewer regions.
Projection changes invalidate certificates. Jobs recheck tile/neighbor revisions
and target detail. Normal play reranks 256 pending meshes per processing call;
removal admission uses an exact maintained coverage count.

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
