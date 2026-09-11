# Gameplay and networking

The runnable entry point is [src/main.cpp](../../src/main.cpp). It initializes
logging, crash handling, and networking, then starts a server for `--server` or
prompts for a human/bot client, character, and address.

## Runtime shape

```text
mc_main
  server mode: GameServer -> authoritative World
  client mode: PlayerClient or BotClient -> GameClient -> local World
                                                | reliable RuntimeNetwork channel 0
                                                v
                                          GameServer callbacks
```

Shared interfaces are compiled once for both endpoints. Clients do not receive
server `PlayerId`s and instead track remote players by character.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) defines a flat
32 by 32 board, the 100 ms `TICK`, players, and byte-valued `Direction`.
`World` owns player lookup, spawn, movement, despawn, and replicated positions.

A valid location is not within the 3 by 3 neighborhood of another player's
cell, including diagonals. This is the collision invariant used for random
spawns and movement. Explicit spawn and `setPlayerPosition` do not apply that
check, so callers own their preconditions. Random spawning can also continue
indefinitely if no valid cell remains.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines
bounded header-selected plans. Legacy `scenario 1` and CoreLang
`@version("0.0.1")` validate source and limits before returning an immutable
plan, so rejection cannot mutate a world or start the runner. Grammar and
examples are in the [scripting guide](../scripting/README.md).

The legacy `flat3d-v1` profile records flat-plane `(x, y, z)` positions and
yaw/pitch/roll degrees for reproducible camera replay. Z must remain zero while
the authoritative `World` is flat. Its camera-relative commands lower with yaw
zero forward at +Y and positive yaw toward +X, then use the unchanged cardinal
`Direction` wire payload. Runtime evidence records the stable replay ID,
camera-input count, and 100 ms server tick separately from presentation cadence.

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) exposes a
`std::variant` with five messages:

| Direction | Message | Payload |
| --- | --- | --- |
| client → server | `JoinRequest` | one selected character |
| server → client | `JoinResponse` | acceptance boolean |
| client → server | `ClientInput` | two direction bytes |
| server → client | `ServerPlayerPosition` | character and cell |
| server → client | `ServerRemovePlayer` | character |

`Message.cpp` prepends a private tag. Decoding requires a known, complete,
valid, non-trailing payload. Directions encode `-1` as `255`; this is a
same-build native-layout protocol with no cross-version contract.

## Server authority

[GameServer.hpp](../../src/server/include/server/GameServer.hpp) creates a
localhost server on default port `20040` (constructor accepts another port);
[GameServer.cpp](../../src/server/GameServer.cpp)
polls once per 100 ms and resets the per-tick moved-character set.

The server rejects duplicate/already-joined characters, privately accepts a
success, broadcasts positions, ignores unjoined input, and allows one nonzero
input per player per tick. Joined disconnect broadcasts removal before despawn.

## Client roles and lifecycle

[GameClient.hpp](../../src/client/include/client/GameClient.hpp) and
[GameClient.cpp](../../src/client/GameClient.cpp) implement the common client
state machine: connect with a one-second timeout, send a join, poll until an
accept/reject/disconnect, then repeatedly poll and render presentation frames.
`FrameScheduler` sends input reliably at the independent 100 ms authoritative
cadence and sleeps at most one millisecond between presentation attempts, which
permits normal windowed refresh without busy spinning or catch-up input bursts.
Position messages update a local `World`; unknown characters receive locally
assigned ids. A local authoritative update centers the first-person eye at
`(x+1, y+1, 1.6)` while retaining camera angles; only remote players become
render records. A disconnect or rejected join stops the loop.

[PlayerClient.hpp](../../src/client/include/client/PlayerClient.hpp) and
[PlayerClient.cpp](../../src/client/PlayerClient.cpp) provide the GLFW/Vulkan
client. Normal gameplay enables the HUD by default. GLFW cursor movement controls local yaw/pitch; W/S and A/D become
camera-relative cardinal directions through the GLFW-independent controller.
This never predicts or applies a local movement result. Each render converts
local players to colored 2 by 2 render records. R reloads the
renderer on a press edge stored per client instance.

`BotClient` renders nothing and changes a persistent random direction with
probability 1/50 per input call. Neither client predicts movement.

## Direct test mapping

Message, world, server, transport, scenario, camera, and scheduler tests are
registered in `mc_tests`. Full UI and cross-process runtime acceptance remain
separate; see [renderer tests](RENDERING.md).
