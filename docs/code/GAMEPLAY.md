# Gameplay and networking

## Scope and checkout state

This note describes the current working tree, including staged and unstaged
changes in gameplay, protocol, and tests. Those changes are not a released
behavioral guarantee. For the committed baseline, inspect `HEAD` alongside the
current diff before making release claims.

The runnable entry point is [src/main.cpp](../../src/main.cpp). It initializes
logging, crash handling, and networking, then starts a server for `--server` or
prompts for a human/bot client, character, and address. Input/address failures
and uncaught exceptions produce a non-zero exit code in this checkout.

## Runtime shape

```text
mc_main
  server mode: GameServer -> authoritative World
  client mode: PlayerClient or BotClient -> GameClient -> local World
                                                | reliable ENet channel 0
                                                v
                                          GameServer callbacks
```

The target combines these sources through
[src/client/CMakeLists.txt](../../src/client/CMakeLists.txt),
[src/server/CMakeLists.txt](../../src/server/CMakeLists.txt), and
[src/shared/CMakeLists.txt](../../src/shared/CMakeLists.txt). The shared
interfaces are compiled once and used on both sides; clients do not receive
server `PlayerId`s and instead track remote players by their unique character.
[ProjectInfo.hpp](../../src/shared/include/shared/ProjectInfo.hpp) supplies the
project identity used to name the human-client window and Vulkan application.

## Shared simulation

[World.hpp](../../src/shared/include/shared/world/World.hpp) defines a flat
32 by 32 board, a 100 ms `TICK`, `PlayerId`, `Player`, and byte-valued
`Direction`. [World.cpp](../../src/shared/world/World.cpp) owns the player
vector and implements:

- `spawnPlayer`: requires a unique character via `ASSERT`; random spawning
  retries until the location is valid, while an explicit location is stored as
  supplied.
- `movePlayer`: interprets each direction byte as `int8_t`, rejects missing
  players and targets outside `[0, WIDTH) x [0, HEIGHT)`, then updates on a
  valid target. A zero direction succeeds without changing position.
- `despawnPlayer`, `player`, `playerByCharacter`, and `setPlayerPosition`:
  linear lookups/mutations of that vector.

A valid location is not within the 3 by 3 neighborhood of another player's
cell, including diagonals. This is the collision invariant used for random
spawns and movement. Explicit spawn and `setPlayerPosition` do not apply that
check, so callers own their preconditions. Random spawning can also continue
indefinitely if no valid cell remains.

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

[Message.cpp](../../src/shared/net/Message.cpp) prepends a private one-byte
tag and serializes each payload with `ByteWriter`; decoding requires a known
tag, a complete payload, no trailing bytes, and valid values. Accepted
characters are `@ # $ % &`; each direction component is `0`, `1`, or `255`
(`255` becomes `-1` during movement); replicated positions must be in bounds.
Encoding itself does not validate its input. Because payloads are copied as
native trivially-copyable structures, this is a same-build protocol with no
declared byte-order, padding, or version-compatibility contract.

To extend it, add the variant alternative, private tag, encoder, decoder,
validation, both endpoint handlers, and round-trip/rejection tests together.
Never renumber an existing tag without coordinating every peer.

## Server authority

[GameServer.hpp](../../src/server/include/server/GameServer.hpp) creates a
localhost server on default port `20040` (constructor accepts another port);
[GameServer.cpp](../../src/server/GameServer.cpp)
polls once per 100 ms and resets the per-tick moved-character set.

On join, the server rejects duplicate characters or an already-spawned connection
with a private reply. Success privately accepts, broadcasts the new position
to all connected clients, and sends existing positions to the newcomer.
Unjoined input is ignored. Zero input does not consume the movement allowance;
the first non-zero input per character/tick consumes it even if collision
rejects movement. Successful movement broadcasts its authoritative position.
Joined disconnect broadcasts removal before despawning; unjoined disconnect
does nothing to world state. There is no server exit path, persistence,
authentication, or rate policy beyond the per-tick movement gate.

## Client roles and lifecycle

[GameClient.hpp](../../src/client/include/client/GameClient.hpp) and
[GameClient.cpp](../../src/client/GameClient.cpp) implement the common client
state machine: connect with a one-second timeout, send a join, poll until an
accept/reject/disconnect, then repeatedly poll, render, send input reliably,
and sleep 100 ms. Position messages update a local `World`; unknown characters
receive locally assigned ids. A disconnect or rejected join stops the loop.

[PlayerClient.hpp](../../src/client/include/client/PlayerClient.hpp) and
[PlayerClient.cpp](../../src/client/PlayerClient.cpp) provide the GLFW/Vulkan
client. Escape stops it; W/S and A/D select one signed byte per axis; each
render converts local players to colored 2 by 2 render records. R reloads the
renderer on a press edge stored per client instance.

[BotClient.hpp](../../src/client/include/client/BotClient.hpp) and
[BotClient.cpp](../../src/client/BotClient.cpp) provide a headless client. It
renders nothing and changes a persistent random direction with probability
1/50 per input call. Neither role predicts movement or reconciles a local
player with a server id.

## Direct test mapping

[tests/core/shared_message_tests.cpp](../../tests/core/shared_message_tests.cpp)
covers every message-kind round trip plus truncated, unknown, trailing, and
invalid decoded payloads. [tests/core/shared_world_tests.cpp](../../tests/core/shared_world_tests.cpp)
covers lookup, collisions, boundaries, random-spawn bounds, zero movement, and
despawn. [Server tests](../../tests/core/game_server_tests.cpp) use real local
ENet clients to cover private replies, newcomer visibility, repeated joins,
idle/multiple input, and joined/unjoined disconnects. Their fixture polls the
server directly; it does not exercise `run()` tick resets or client scheduling.
[Transport tests](../../tests/core/net_client_server_tests.cpp) cover connections,
channels, echo, relay and disconnect behavior, including explicit multi-client
connection readiness before echo. [tests/CMakeLists.txt](../../tests/CMakeLists.txt)
registers these in `mc_tests`. Main prompts, the full client loop and cross-process
runtime still need separate acceptance; see [renderer tests](RENDERING.md).
