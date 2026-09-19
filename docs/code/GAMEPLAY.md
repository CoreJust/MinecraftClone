# Gameplay and networking

The runnable entry point is [src/main.cpp](../../src/main.cpp). It initializes
logging, crash handling, and networking. Launch uses `--server [--port PORT]`,
`--player-client`, or `--bot-client`; clients accept `--address IP:PORT`.
No arguments start a graphical localhost player; Flight joins select free internal tokens automatically.

## Runtime shape

Clients track remote players by character rather than server `PlayerId`.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) defines the 100 ms
`TICK`, byte-valued normalized XYZ `Direction`, and players with deterministic
10,000-subcell remainders. `World` owns player lookup, spawn, fixed-step
movement, despawn, and replicated positions. Flat mode retains the 32 by 32
board and its two-dimensional collision rules. Flight mode uses signed XYZ
coordinates, a fixed clear-air spawn, bounds checks without gravity or player
collisions, and normalized three-axis movement. A normal tick advances 0.56
cells at full direction magnitude; the remainder persists across ticks. Each
authoritative player has a 2 by 2 footprint, so its origin is limited to cells
0 through 30 inclusive (0 through 300,000 subcells) on both axes; its
footprint may end at, but never exceed, the platform edge.

A valid location is not within the 3 by 3 neighborhood of another player's
cell, including diagonals. This is the collision invariant used for random
spawns and movement. Explicit spawn and `setPlayerPosition` do not apply that
check, so callers own their preconditions. Random spawning can also continue
indefinitely if no valid cell remains.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines
bounded plans loaded only from CoreLang `@version("0.1.2")` sources. The host
validates source and limits before returning an immutable plan, so rejection
cannot mutate a world or start the runner. Grammar and
examples are in the [scripting guide](../scripting/README.md).

The legacy `flat3d-v1` profile records flat-plane `(x, y, z)` positions and
yaw/pitch/roll degrees for reproducible camera replay. Z must remain zero while
the authoritative `World` is flat. Its camera-relative commands lower with yaw
zero forward at +Y and positive yaw toward +X, then use the unchanged cardinal
`Direction` wire payload. Runtime evidence records the stable replay ID,
camera-input count, and 100 ms server tick separately from presentation cadence.

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) exposes a
`std::variant` with five versioned, fixed-width little-endian messages:

| Direction | Message | Payload |
| --- | --- | --- |
| client → server | `JoinRequest` | character, world mode, and configuration identity |
| server → client | `JoinResponse` | acceptance boolean |
| client → server | `ClientInput` | three signed normalized direction bytes (`-127..127`) |
| server → client | `ServerPlayerPosition` | character, XYZ cell, and three subcell remainders |
| server → client | `ServerRemovePlayer` | character |

`Message.cpp` prepends a magic byte, protocol version, and tag. Decoding
requires a known, complete, valid, non-trailing payload and rejects old or
mixed versions before any authority mutation. The join configuration identifies
the selected seeded world and the server rejects mismatches before gameplay.

## Server authority

[GameServer.hpp](../../src/server/include/server/GameServer.hpp) creates a
localhost server on default port `20040` (constructor accepts another port);
[GameServer.cpp](../../src/server/GameServer.cpp)
drains pending events within each 100 ms tick, processes at most one queued
input per player, rejects joins whose mode or configuration differs from its
authoritative world, and includes the acknowledged input sequence plus a
monotonic state revision in each replicated position.

The server rejects duplicate/already-joined characters, privately accepts a
success, broadcasts positions, ignores unjoined input, and allows one nonzero
input per player per tick. Joined disconnect broadcasts removal before despawn.

## Client roles and lifecycle

[GameClient.hpp](../../src/client/include/client/GameClient.hpp) and
[GameClient.cpp](../../src/client/GameClient.cpp) connect, join, poll, and
render. `FrameScheduler` sends input every 100 ms and sleeps at most one
millisecond between presentation attempts. Position messages update a local
`World`; unknown characters receive locally assigned ids. The first presentation
update snaps; later updates interpolate during one `TICK` and gaps hold the last
position. A disconnect or rejected join stops the loop.

[PlayerClient.hpp](../../src/client/include/client/PlayerClient.hpp) and
[PlayerClient.cpp](../../src/client/PlayerClient.cpp) provide the GLFW/Vulkan
client. Normal gameplay enables the HUD by default. GLFW cursor movement controls local yaw/pitch; W/S and A/D become
camera-relative normalized horizontal directions through the GLFW-independent controller.
The shared client predicts only its local player's queued input, then rebuilds
that prediction from acknowledged server state; it never mutates the
authoritative `World`. Each render samples
every received player presentation into colored 2 by 2 render records and
centers the third-person camera from the sampled local presentation. R reloads
the renderer on a press edge stored per client instance.

`BotClient` renders nothing and changes a persistent random direction with
probability 1/50 per input call.

## Direct test mapping

Message, world, server, transport, scenario, camera, and scheduler tests are
registered in `mc_tests`. Full UI and cross-process runtime acceptance remain
separate; see [renderer tests](RENDERING.md).

## S5 chunk data

`Chunk` stores 4096 Air/Stone IDs in a 16-cubed unit with signed chunk
coordinates and checked local coordinates. Bulk construction rejects unsupported
IDs; effective edits update revision and content hash. The retained S5
`CanonicalWorld` embeds its historical CoreLang 0.0.3 seed-42 generator at
configure time, while S6 scenario and terrain scripts require CoreLang 0.1.2. `ScriptedWorld`
collects bounded callbacks into a private candidate and publishes it with its
configuration identity only after coordinate, block, operation-budget, and
completion validation succeed. There is no separate script-fuel setting.

## S5 exposed-face mesh

`ChunkMesher` emits local origin, material and outward direction for each
exposed face in stable order. Optional neighbor boundary planes suppress solid
seams. Cache validity compares the exact block and neighbor inputs plus revision;
a content hash alone never authorizes reuse. Repeated identical updates preserve
the mesh and build count. Renderer integration is tracked separately.
