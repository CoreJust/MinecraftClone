# Gameplay and networking

Run [src/main.cpp](../../src/main.cpp) with `--server [--port PORT]`,
`--player-client`, or `--bot-client`; clients use `--address IP:PORT`. No
arguments launch a graphical localhost player. Flight joins select free tokens.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) defines the 100 ms
`TICK`, byte-valued normalized XYZ `Direction`, and players with deterministic
10,000-subcell remainders plus a validated four-bit palette identity. `World` owns player lookup, spawn, fixed-step
movement, despawn, and replicated positions. Flat mode retains the 32 by 32
board and its two-dimensional collision rules. Flight mode uses wrapped XYZ
coordinates and a fixed clear-air spawn. The independent movement capabilities
allow flight, collision bypass, or both; flight without collision bypass sweeps
terrain and other player bodies with tangential wall sliding, while players
without flight use gravity, terrain support, jumps, and the same player-body
collision rules. Remainders persist across ticks. Each
authoritative player has a 2 by 2 footprint in flat mode and a 10/16-cell
footprint in flight mode, so its origin is limited to cells
0 through 30 inclusive (0 through 300,000 subcells) on both axes; its
footprint may end at, but never exceed, the platform edge.

Spawn and movement enforce player separation.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines
bounded CoreLang `@version("0.1.2")` plans. The host validates source and limits
before publishing an immutable plan; rejection cannot mutate a world or start
the runner. Grammar is in the [scripting guide](../scripting/README.md).

The legacy `flat3d-v1` profile records flat-plane `(x, y, z)` positions and
yaw/pitch/roll degrees. Z must remain zero while
the authoritative `World` is flat. Its camera-relative commands lower with yaw
zero forward at +Y and positive yaw toward +X, then use the unchanged cardinal
`Direction` wire payload. Runtime evidence records the stable replay ID,
camera-input count, and 100 ms server tick separately from presentation cadence.

## Permissions

The server publishes generation-tagged movement capability snapshots; policy
declarations, compilation limits, selector resolution, and precedence are
documented in [POLICY.md](POLICY.md).

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines versioned,
little-endian join, input, player-position/removal, and height-tile streaming
messages. Inputs carry normalized three-axis direction, horizontal view heading,
and a sequence; positions carry authoritative coordinates, subcell remainders,
palette identity, and acknowledgement. The server assigns a stable
pseudo-random palette index from the character identity, so reconnecting with
that identity receives the same one of the documented 16 opaque colors.

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

The server rejects duplicate characters, ignores unjoined input, and allows one
nonzero input per player per tick. A joined disconnect broadcasts removal.

Height-tile delivery uses bounded credits and cleanup capacity. Movement
responses precede bulk terrain, whose dispatch rotates across clients.

## Client roles and lifecycle

[GameClient.hpp](../../src/client/include/client/GameClient.hpp) and
[GameClient.cpp](../../src/client/GameClient.cpp) connect, join, poll, and
render. `FrameScheduler` sends input every 100 ms and sleeps at most one
millisecond between presentation attempts. Position messages update a local
`World`; unknown characters receive locally assigned ids. The first presentation
update snaps; later updates interpolate during one `TICK` and gaps hold the last
position. A disconnect clears connection state and retries the join while the
game remains alive.

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

## S6 moving terrain

Residency is a camera-independent radius-45 circle. Generation fills radius-3
and radius-8 circles, then a directional ellipse reaching the outer circle.
Rate-limited background work completes the circle after the player settles.
Sixteen stable heading sectors affect priority only; rotation never changes
residency. The same ordering drives bounded server generation and client meshing.
CoreLang 0.1.2 supplies wave parameters once; C++ evaluates heights and stone.
Terrain has six base layers, 128-block spacing, and a first-wave spawn.
HUD acceleration profiles are 2x, 3x, 5x, 8x, 15x, 30x, 80x, 200x, and 500x.

## S5 chunk data

`Chunk` stores checked 16-cubed Air/Stone IDs and tracks revision/content hash;
S6 generation uses CoreLang 0.1.2; sparse-world scripts use CoreLang 0.1.3
through the bounded `SparseWorld`.

## S5 exposed-face mesh

`ChunkMesher` emits stable exposed faces. Neighbor planes suppress seams; cache
validity requires exact blocks, neighbors, and revision, not a content hash alone.
