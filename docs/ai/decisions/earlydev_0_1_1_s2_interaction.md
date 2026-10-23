# S2 interaction and permission design

This proposal defines aiming, breaking, placement, material selection, and effective-rights display. The [S2 design index](earlydev_0_1_1_s2.md) owns the full requirement hierarchy and release gates; world storage, replication, rendering, and verification are covered separately. Nothing here claims implementation or qualification.

## Accepted modes and policy composition

The owner confirmed **Creative can break Bedrock; Survival cannot fly.** Flight, bypass and breaking presets follow the roadmap with those clarifications. Placement defaults remain proposals; hard server restrictions always take precedence.

| Base mode | Flight / bypass | Editing | Bedrock |
|---|---|---|---|
| Spectator | Flight and bypass defaults | No breaking or placement | Denied |
| Creative | Flight, no bypass | Quick break; broad placement | Break allowed |
| Survival | No flight or bypass | Timed break and filtered placement | Break denied |

`WorldMode::Flat/Flight` remains topology, separate from `GameMode`. The server chooses the base mode and applies preset/policy before initial publication. Creative is a proposed local-world startup default; remote sessions use the server's choice. Mode changes are server-owned unless `mode.change` authorizes a player request.

Compose rights from base-mode defaults, applicable soft policy using current specificity and restrictive tie rules, then hard bounds that only tighten. Show the mode and precise effective permissions in parentheses, including flight, bypass, edit filters and speed. If trusted soft policy grants a non-preset right, show `Custom (base=Survival)` and its effective rights. A hard restriction changes effective rights, not the base label.

The current policy schema has scalar capabilities, so represent filters with independent boolean keys such as `block.break.<material>` and `block.place.<material>`. Combine the action right, its filter, the material key, and hard restrictions. An empty whitelist denies all materials for that action; an empty blacklist adds no material denial. Keep break and place filters separate. Proposed defaults: Survival cannot break or place Bedrock; Creative quick-break permits Bedrock unless hard policy denies it; Spectator cannot edit. Survival placement of all current solids except Bedrock and Creative placement of all current solids remain proposals. Never place Air.

## Aim, progress, and placement

Use the screen-center candidate in first-, rear-, and front-camera views, without trusting client camera origin or hit cell. The proposed reach is 5 blocks from the actor's eye. The server reconstructs a bounded shared camera from accepted pose, perspective, and validated 3D aim, then checks eye-origin reach and unobstructed eye-to-hit visibility. Reject a client target hint that differs from the server result. Match the renderer's curvature-to-logical-voxel transform or the centered highlight may differ from the edited cell. Third-person framing may affect the candidate but cannot add reach or bypass eye visibility. Camera reconstruction and reach are proposed defaults.

Traverse logical cells with deterministic voxel DDA; advance all axes tied at an exact edge or corner. `Unknown` stops without materializing data, and preview-only cells are not editable. Reject out-of-bounds cells, resolve horizontal wrapping before mutation, and derive placement face from the server traversal. The world note owns sparse-cell and boundary details.

At the 100 ms server cadence, break durations are exact: Snow 1.0 s (10 complete intervals), Dirt/Grass 1.5 s (15), Ice 2.0 s (20), Stone 3.0 s (30). The accepted start tick earns no interval. Proposed quick break commits on the first authorized server tick. Limit edits to one per player per tick; no client clock or progress is authoritative.

Bedrock has no normal-break completion; only authorized quick breaking can remove it.

Keep progress through small aim changes while the target cell stays the same. Cancel on target/material, cell-version, mode/policy, visibility, or reach change; also on release or lease expiry. The proposed 300 ms lease is renewed by sequenced held-break input. Place once per right-button/touch press, without held repeat. Validate selected material and face, then atomically require a known Air cell and no collidable-player/entity overlap. World versioning and edit replication are specified separately.

## Controls and safe transitions

Proposed desktop controls: left mouse hold breaks, right mouse press places, and **B** cycles permitted solids. **F6** remains the movement-capability cycle and cannot grant a denied right. **G** requests a mode change only with `mode.change`; local play may grant it, while remote play denies self-change unless authorized. **M** opens the [world map](earlydev_0_1_1_s2_maps.md); entering cancels edits and owns navigation input. No inventory or mode menu is in scope.

Android needs break-hold, place-press, and next-block controls; expose mode requests only when allowed. Each touch pointer has one owner, so edit taps do not steer the camera. Cancel holds on `ACTION_CANCEL`, pause, focus loss, or disconnect; server lease handles a lost release. A loading gesture held when gameplay opens cannot become a new edit edge.

Evaluate mode changes atomically at a server tick. Reject a mode that embeds or overlaps the player, retaining prior mode, rights, and pose. Flight-off while airborne uses ordinary gravity; no teleport or silent relocation. S4 owns embedded-player recovery.

Source anchors: [Policy.hpp](../../../src/shared/include/shared/policy/Policy.hpp) lines 19–78, 273–298 and [CoreLangPolicy.cpp](../../../src/shared/policy/CoreLangPolicy.cpp) lines 1227–1348 cover scalar rights and precedence; [World.hpp](../../../src/shared/include/shared/world/World.hpp) lines 16, 43–74, 124–146 and [GameServer.cpp](../../../src/server/GameServer.cpp) lines 1162–1178, 3094–3119 cover cadence, topology, and input; [PlayerPresentation.cpp](../../../src/client/PlayerPresentation.cpp) lines 336–364 covers camera views. The [source record](earlydev_0_1_1_s2_sources.json) hashes the observed dirty S1 tree at `46cb6132f54a435f9aae8381b189e847fbc22591`; recheck after S1 delivery.
