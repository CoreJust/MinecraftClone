# S2 map data and composition

The accepted prototype at `232eae68` defines appearance/controls;
[map behavior](earlydev_0_1_1_s2_maps.md) defines product requirements.
This is the native data/composition design. Implementation integrates the final
S1 atlas and surface interfaces, not the browser's seed42 fixture.

## Source and coordinates

`MapSource` is an immutable view over one session epoch, model/config identity,
topology, grid extent, available fields and source revision. Adapt the accepted
session atlas through public client sources. Local detail adds canonical top
material/height and coverage/edit versions. Never generate/query the whole
voxel world for a map, or mix independently seeded layers.

Normalize wrapped world XY before sampling. With current 65,536-block period,
UV is normalized XY divided by 65,536; native raster rows increase with Y,
so North is negative Y. Heading is clockwise from North and converts to native
yaw using 180-heading. Keep conversion at the source/view boundary, shared by
minimap, world map and marker telemetry. Player Z remains unrelated to
geological elevation. New terrain models retain the world topology contract.

One `MapView` holds center, zoom, projection mode, selected layers and legend
state. World map close view covers the usable frame isotropically with cropped
edges; wrap adjacent geography when crossing seams. `0` fits one whole world
with quiet gutters, `R` restores player-centered close view, `L` toggles details.
Do not repeat multiple worlds in overview or stretch the square grid to fit.
Minimap uses an independently sized north-up local L1/L2 window, centered on
the player with prominent N/E/S/W and heading.

## Layers and legends

| Key | Source and meaning |
|---|---|
| P | Plate IDs/boundaries and drift vectors; motion direction, not physical speed. |
| H | Geological elevation in km relative to sea 0; shared relief/contours. |
| T | Temperature in C; continuous translucent scalar field. |
| B | Coarse climate zones until a canonical biome producer exists; label that limitation. |
| W | Wind vector pair and direction key with actual model units. |
| C | Current vector pair, sampled only over water; distinct vector key. |
| O | Model moisture reservoir, explicitly not relative humidity. |
| Q | Model precipitation, explicitly not mm/year. |

Each layer has enabled/disabled plus availability, units and a numeric/color
or vector key. Missing either vector component makes the pair unavailable.
Legends use the same ranges/transforms as pixels; retain stable ranges for an
immutable source so pan/zoom cannot silently recolor the same value. Preserve
finite zero values. Invalid/nonfinite fields produce explicit unavailable
state and cannot poison image buffers or another layer.

Port the accepted pure raster composition: geographic land/water base,
H relief, B climate colors, shared display ice, translucent T/O/Q, then plate/
vector annotations and player/cardinal glyphs. T never secretly replaces B
when another toggle changes. Blend enabled T/O/Q colors with equal weight;
initial total opacity is 44%/54%/60% for one/two/three fields. Q uses the accepted
log transform with matching legend. All-off retains neutral geography; all-on
keeps every enabled layer identifiable. Map-backed legends use fixed contrast;
unbacked gameplay text follows its adaptive styling contract.

## Local edits and publication

Reuse the canonical highest-occupied-column projection from the map behavior
note: roofs hide tunnels, Unknown differs from Air. Keep verified base L1/L2
visible outside affected edited cells. An edited cell cannot claim current
detail until matching epoch/base, column manifest and edit versions are present;
render it pending/Unknown meanwhile. Never require global voxel readiness.

An accepted top edit invalidates the affected column/tile and wrapped neighbors;
removal exposes the next proven solid. Worker samples use immutable source
snapshots; before publication compare their dependency versions. Drop stale
results atomically rather than attaching newer labels to older pixels. Pan,
zoom and layer changes alter view/composition, not canonical terrain or meshes.

Cache identity includes epoch/model/source revision, layer/availability mask,
palette/range identity, and tile coordinates/scale plus relevant edit versions
for detailed tiles. Marker/heading are separate draw-time overlays; movement
does not invalidate the whole atlas. Bound local sampling initially to 256
columns per update, following the existing ColumnMapPresentation cap; coalesce
repeated requests and preserve the last compatible image with pending status.

Emit `MapDraw` values containing immutable image handle/revision, source crop,
destination rectangle, scissor and sampling choice. Renderer map commands add
viewport/crop support to today's fullscreen path, not a new UI toolkit. Keep
pixel-art nearest sampling initially. Image/cache/upload limits, fence retirement
and reset are owned by the [presentation architecture](earlydev_0_1_1_s2_presentation.md).
Generation's final raster uses this same palette/ice implementation.

## Evidence required

Compare all 256 layer combinations, availability masks, scalar endpoints,
log legends, alpha mixes and source immutability. Test UV/heading, negative
coordinates, wraps, close/overview, long legends and input ownership. Verify
edit/eviction/rejoin and stale-worker behavior against canonical top surfaces.
Measure native sampling, cache/upload memory, frame cost and hidden-map work
at ordinary desktop/Android settings; browser coverage is design evidence.
