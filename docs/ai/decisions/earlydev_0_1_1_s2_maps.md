# S2 — Minimap and world map design

**Status:** The owner accepted the prototype at `232eae68`; its layout, layers and keys are the S2 baseline. Native implementation and qualification remain open.

## Current paths and scope

The [S2 index](earlydev_0_1_1_s2.md) owns scope. This note owns behavior; [map pipeline](earlydev_0_1_1_s2_map_pipeline.md) specifies source/composition/cache contracts, and [UI](earlydev_0_1_1_s2_ui.md) owns screens/telemetry.

The following source locations are sampled S1 worktree observations, recorded with hashes in the [source record](earlydev_0_1_1_s2_sources.json); several files are not committed yet. Reconcile them with final S1 before implementation.

`ColumnMapPresentation` (`src/client/include/client/ColumnMapPresentation.hpp:51`) is a CPU sampler capped at 256×256 columns and 256 samples per update. It uses verified surface data or a non-materializing world query; incomplete columns stay Unknown (`src/client/ColumnMapPresentation.cpp:77`). Its separate `ColumnMapClient` (`src/client/ColumnMapClient.cpp:165`) does not present a GPU game frame. `GenerationMapPresentation` (`src/client/include/client/GenerationMapPresentation.hpp:22`) is diagnostic: PlayerClient feeds it only to the fullscreen loading overlay, then clears it on gameplay start (`src/client/PlayerClient.cpp:3878`, `src/client/render/shaders/map.frag:18`). S1 excluded an ordinary-play navigable map ([loading note](earlydev_0_1_1_s1_loading.md#L4)); the owner’s S2 addition supersedes that boundary. Keep gameplay maps separate from loading readiness.

## Proposed behavior and data

Use independent keys: `P` plates, `H` heights, `T` temperature, `B` biomes,
`W` wind, `C` currents, `O` moisture and `Q` precipitation. Show states,
units/ranges and color/vector keys. T/O/Q are translucent continuous heatmaps
preserving terrain; H adds relief. Biomes mean coarse climate zones, moisture
is not relative humidity, and model rain is not mm/year. Use one matching
atlas identity. Generation ends on this exact normal terrain raster. In this
prototype, retained ice uses nonwater Snow zones and water at/below -10C;
the expanded legend identifies this visual proxy, not native freezing physics.

Give the map the usable viewport, with compact backed text legends; `L`
expands details. Default may show a closer pan-able region; `0` fits the whole
world. Preserve isotropic projection, never stretch geography to fill space.

Place a north-up minimap at top-right, showing the player in the shared selected-model L1/L2 projection around them, with prominent outlined N/E/S/W labels and a heading marker, using the game's material palette rather than the diagnostic atlas colors. It must cover more than resident voxel columns. The S1 scaffold’s L1/L2 spacing is 64/16 blocks at 256 atlas resolution and 32/8 at 512 (`docs/ai/decisions/earlydev_0_1_1_s1_lod_scaffold.md:21`). Shared `TerrainLodArtifact` (`src/shared/include/shared/world/TerrainLod.hpp:41`) records model identity and level; today’s model is an S1 placeholder. Use the eventual accepted terrain identity and shared projection, not that placeholder or duplicate terrain research. Use the accepted immutable session atlas for the wider world map ([atlas contract](../WORLD_MAP.md#L3)).

Propose M to open/close the larger world map, arrows/drag to pan, +/- or pinch/wheel to zoom and R to reset. W toggles wind; do not also bind WASD pan. Suppress toggle repeat and pass modified browser shortcuts through. Show plain text instructions; no button/widget toolkit. M is unbound in the surveyed PlayerClient path; check all input routes before implementation. Android map actions and pointer ownership follow the [native architecture](earlydev_0_1_1_s2_presentation.md). Map input cannot move, look, edit or change mode; hide the reticle while multiplayer continues. Show the local player only. Map clicks do not edit or teleport; no fog, waypoints or other-player disclosure is required.

Use shared L1/L2 projection as the local base without waiting for L3 voxels. Overlay local edits only at map scales where block-surface detail is meaningful. Gate each affected cell on current model identity, epoch, and edited-column manifest/revisions ([replication](earlydev_0_1_1_s2_replication.md#L15)); show incomplete edited cells as pending/Unknown while unaffected L1/L2 remains visible. Never require the whole map view to be voxel-ready, or trigger whole-world generation/download. At detail scale, define the projection as the highest occupied block per XY column: after an accepted top-block removal, recompute the next solid; show Air only when the column is proven empty. The top view cannot show tunnels or undercuts, and roofs hide lower geometry. Unknown is distinct from Air.

Key tile/cache entries by epoch, base/model identity, relevant revisions, and tile coordinates/zoom. Authoritative edits invalidate only affected map columns/tiles, including wrapped neighbors. Movement, pan, and zoom alter the view window, not terrain identity or edit meshes. Reset stale map state on session/base change. Reuse verified public surface/LOD views; bound raster work, pixel/tile caches, and background scheduling, starting from ColumnMapPresentation’s current caps pending measured need.

## Rendering and acceptance

Draw both maps in the final HUD layer from the [S2 rendering design](earlydev_0_1_1_s2_rendering.md), using a rectangular map image and bitmap labels, without panel chrome. Use fixed-contrast text in compact backed legend areas; adapt only unbacked HUD text to the pre-HUD scene. A bounded destination viewport/scissor extends today's fullscreen map draw; it is an explicit S2 change. Preserve output encoding and consume a typed UI snapshot. Resize updates layout; device recreation rebuilds resources and discards incompatible caches. Offscreen/native captures share this route.

Acceptance covers broad L1/L2 visibility before L3 residency; N/E/S/W plus heading; negative coordinates and wrap seams; pan/zoom/recenter/close; resize/reload and identity reset; Android gesture ownership; and ordinary multiplayer continuity. At a chunk edge, verify an accepted top edit updates only after matching overlay coverage, with no stale base result. Check pending versus Air, removal revealing the next solid, and absence of map-triggered world actions. Measure cache memory, sample work, update time, and same-workload CPU/GPU/network overhead with maps hidden, minimap open, and world map open. Preserve S1 targets; make no unmeasured FPS claim. These are future checks, not results.
