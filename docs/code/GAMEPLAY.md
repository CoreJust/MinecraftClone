Use [src/main.cpp](../../src/main.cpp) with `--server [--port PORT] [--render-distance CHUNKS]`, `--player-client`, or `--bot-client --address IP:PORT`. No args or `--local-game` starts a temporary loopback server/window. `--player-client-2d ... --evidence DIR` records logical CPU column-map frames. `mc_column_map` omits the selector.

## Shared simulation

[World](../../src/shared/include/shared/world/World.hpp) manages lifecycle/replication with 100-ms ticks, normalized XYZ, 10,000-subcell remainders and four-bit palettes. Flat: 32×32. Flight wraps XY, spawns aloft. Entity Z 0–65,535; canonical terrain 0–2,047 (2,048-cell depth). High-flight empty coverage: Ready. Flight/bypass select terrain/body collision; walking adds gravity/jumps; takeoff preserves impulse. Footprints: Flat 2×2, Flight 10/16; spawns enforce separation. Shift descent defers terrain only after bounds, other-player and start-height checks; vertical sweep resolves support.

## Deterministic scenario plans

[Scenario.hpp](../../src/shared/include/shared/scenario/Scenario.hpp) defines CoreLang `@version(0.1.5)` plans. Hosts validate backend-comparison program/resource budgets prepublication; rejection starts no runner/world mutation. `flat3d-v1` stores position/yaw/pitch/roll (Z=0; yaw 0 faces +Y, positive +X). Evidence separates replay IDs, inputs, ticks and presentation; vertical camera defaults to zero ([scripting guide](../scripting/README.md)).

## Permissions

Bypass requires flight ([POLICY.md](POLICY.md)). [Interactions](BLOCK_ACTIONS.md) define permissions, reach and timed breaks.

## Wire protocol

[Message.hpp](../../src/shared/include/shared/net/Message.hpp) defines protocol 28.
Authoritative actions/results, revisioned deltas, sparse edit snapshots/manifests.
Nonce sessions validate epoch/token. Profiles certify columns;
edits require manifested Z-chunk revisions. Dense credits/receipts stay separate.
Bulk/control channels `1/0`; peer cap `49,152 B`; old-epoch debt stays charged.
Startup class windows: `16,384 B`. Post-start canonical precedes dense refill; borrowing preserves edit/preview
reserves, interest and authority fences. Coverage actual/visual: 640/320; caches: 48/80. Inputs:
XYZ/heading/sequence; positions: subcells/palette/ACK. IDs 0–6 fixed; Sand 7;
Older protocols are rejected. Progress pairs wind/current; V9 tiles carry elevation/biome/coast.
Rights allow IDs1–7; capabilities0/1/3. Invalid input/stale ACKs cannot mutate state.

## Server authority

[GameServer](../../src/server/GameServer.cpp) defaults to port `20040`. Polls drain
≤256 events/ms; active/idle waits are 1/5 ms. One block decision/player/tick;
eligible instant actions can commit during polling ([contract](BLOCK_ACTIONS.md)).
Idle gravity advances revisions without ACKs. Invalid/duplicate joins and pre-join
inputs fail; disconnects remove players.

Sequenced intents validate session, accepted eye aim, reach, visibility, mode/rights
and cell version; hard vetoes win. Break credit resets on target/version/policy
change, release or lease expiry. Placement rejects body overlap and duplicate
sequences. Loopback requests use local grants; remote requests require
`minecraft:mode.change`.

Preview batches cap at 64 operations/12,998 bytes and fit a maximum 16-bit tile.
Eight credits share 34,356 bytes within ENet's 65,536-byte window. Removals reserve
capacity; interest, heading and ACKs reorder work; missing entries rotate fairly.
Preview cancellation spares authority jobs. Session/epoch and replay-safe credits stay separate.

## Terrain-v2 preparation and authority

[GENERATION.md](GENERATION.md) owns atlas/regional sampling. `WorldPreparation`
publishes immutable `TerrainWorld` stage snapshots shared by `HeightTileWorkerPool`
previews/chunks (128-job limit). `GameServer` schedules, rejects stale results and
publishes. Ready omits replayed snapshots, retaining client data. LocalGame reuses the world for client L0; remote clients prepare the
negotiated session.

Prepared joins keep T0. Dense 48 permits prompt/start. Full Ready requires 768
startup-reserve profiles, then current 640. Begin/restart/cancel or complete-profile
loss rearms `startup_prefetch_primed_at`. Partial loss preserves current coverage.
Publication checks session/epoch/identity. See [materialized prefetch](GENERATION.md#bounded-materialized-prefetch).

Default Flight joins choose nearest safe dry land by wrapped distance to world
center, breaking ties deterministically. Asynchronous support. Explicit spawns are validated. See [nearest-land spawn](GENERATION.md#nearest-land-prepared-spawn).

Preview meshes need connected near-camera coverage and cardinal neighbors.
Interest changes remesh from retained canonical data.

Preparing/Connecting overlay map progress. Ready shows the final map and prompt.
After that frame, fresh input requests gameplay, which waits for ACK. Focus loss
releases cursor capture. Pixels stay Unknown without matching metadata and authority.

## Client

Creation history: [field contract](CREATION_HISTORY.md).

`GameClient`: cancellable 25 ms polls, 250 ms retries, 30-second peers.
Availability scans active keys allocation-free and requires authority. `FrameScheduler`: 100 ms ticks, skipping misses. Interpolation
holds gaps over a tick; flight Z is linear; gravity/collision use velocity-continuous
Hermite. Prediction replays pending input after ACKs.

`TerrainLodRuntime` rebases concurrent local/distant jobs. Ordinary jobs admit
targets inside the presented disk. At its edge, current-first can mix the validated
current footprint with retained columns. A validated forward capture can extend
it before settle rebuilds the centered disk. Unsupported layouts trigger full
preparation. Guards check sources, epoch, current center and presented footprint.
Edits repair transition rims. Startup and movement qualify separately.

Hooks collect render timings outside benchmark mode.

```cpp
hooks.isolate_platform_input = true;
// A null snapshot restores ordinary focus/release handling.
hooks.block_button_snapshot = [fresh = true]() mutable -> std::optional<client::BlockButtonSnapshot> {
    client::BlockButtonSnapshot buttons{.left_held = true};
    buttons.left_pressed = fresh; // First sample presses; later samples keep holding.
    fresh = false;
    return buttons;
};
```
 [Qualification](../ai/GAMEPLAY_QUALIFICATION.md)
separates S1's turning x30/640-profile route from generic 320 geometry and x200
stress. T0 never resets; legacy 180-second warmup is non-gating.

[PlayerClient](../../src/client/PlayerClient.cpp) uses GLFW/Vulkan. Camera-relative WASD/mouselook. LMB holds break; RMB places immediately/every 200ms.
Held LMB resumes after target gaps. Release or focus/map/mode/session changes
require another press. Shift+Space cancel vertical motion until either releases.
HUD temperature polls one exact asynchronous world/session query with 250 ms
refresh and 1 s expiry. Rendering never waits. Targets show material name/ID.
B cycles blocks; G requests mode.
Zero-size framebuffers keep polling until close. Mac/Windows use an unattached
borderless work area. F2 enables readback. F5 cycles views and first-person
hides the body. R reloads.

## Canonical chunks and mesh

`Chunk` validates materials/identity. Remote `SparseWorld` reads preserve Unknown
without generation. Profiles/edits cover 128 sections through solid 2,047;
manifests allow 64 entries. Collision/support and LOD captures share edit-revision
fences. Dense-cache churn preserves unedited tile identity; edits, deletion and
replacement invalidate it.

Voxels advance cell/chunk versions. Receipt-gated deltas/snapshots preserve
Air/base-reset overrides through eviction/base arrival. Stale updates fail;
bounded resync repairs gaps. An atomic 65,536-cell cap charges at least one cell
per chunk. Neighbor revisions invalidate captures.
