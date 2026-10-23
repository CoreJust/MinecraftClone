# S2 generation presentation

Implement the owner-accepted six-stage geographical sequence from prototype
`232eae681ddd9f38e6187a109168d70ce0b64ed5`. This specifies presentation, not
new tectonic, ice or weather simulation. Preserve the accepted north-up world,
palette, persistent ice and final world-map identity. The
[presentation architecture](earlydev_0_1_1_s2_presentation.md) owns lifecycle,
input and resources; [S1 loading](earlydev_0_1_1_s1_loading.md) owns readiness.

## Producer contract

Adapt existing `WorldLoadingStatus`, generation snapshots and immutable atlas
publications. Add a bounded `GenerationPreview` envelope where fields are absent:
join epoch, model/config identity, producer sequence, actual stage/completed/
total, source grid/projection, available fields and immutable raster/checkpoint.
Progress denominators exist only when the producer supplies them. The sequence
orders presentation updates; it is not an edit revision or elapsed percentage.

Plate IDs/vectors, elevation/water masks, climate/temperature, moisture/rain
and wind/current pairs must share one checkpoint identity. Optional fields have
availability bits; missing fields suppress their effect, never substitute
unrelated noise or another world's data. Extend the public snapshot at its
producer boundary when needed; do not inspect half-mutated generator arrays.
Existing snapshots do not expose every final-atlas channel, so this is S2 work.

Initial preview publication cap is 8 Hz at 128x128; keep one latest immutable
checkpoint and one pending consumer request, coalescing superseded updates.
Reuse existing session/atlas transport; any added remote preview payload needs
bounded size/rate, sequence/epoch validation and transport tests in W1/W5.
Preview loss must not block generation, snapshot delivery or gameplay packets.
Join-existing displays synchronization/coverage, not a claim of recreating it.

## Presentation sequence

| View | Available data and visual action |
|---|---|
| Tectonics | Translate coherent plate patches using matching plate vectors; small bounded offsets, no per-cell stretching or unrelated colored seams. Show colliding/rifting coast changes. |
| Relief | Grow available relief/islands and settle plate motion; retain the same projection and recognizable geography. |
| Water | Advance coasts, rivers and basins toward canonical final masks; this does not create S3 fluid voxels. |
| Ice | Advance only toward the shared final display mask, ordered by temperature; retain reached ice through later stages. |
| Climate | Introduce matching biome/climate colors, sparse wind/current paths and rain over wet fields. Currents are water-only; effects skip retained ice. |
| Arrival | Remove transient motion/weather; use the exact normal map raster and mark the validated start location. |

These six views are not six new mandatory generator stages. Bind them to the
real producer's available phases/checkpoints; skip or settle unavailable
transitions. The prototype's 30-second schedule and Enter-to-advance controls
are review fixtures, not production requirements. Never replay a whole
cinematic after real readiness. On ready, settle immediately to the exact final
map and present the key gate. Failure/cancellation replaces animation promptly.

When actual intermediate checkpoints exist, interpolate only compatible fields
from the same identity. Otherwise bounded illustrative deformation may explain
the current phase without claiming historical simulation. Keep actual progress
text separate from visual time; no fake percentage/ETA. A slow or absent image
cannot prevent truthful stage/error/readiness text.

## Shared geography and animation rules

Generation and world map use one pure palette/composition implementation, map
projection and versioned final raster. Minimap uses the same identity but local
material/surface colors. No screenshots are analytical layer sources.

The accepted fixture derives display ice from nonwater Snow zones and ocean/
lake cells at/below -10 C. Preserve it as an explicitly labeled climate proxy
until the accepted production model supplies a canonical ice display field.
It is not freezing physics, voxel placement or a required physical threshold.
The sampled S1 diagnostic uses a different ocean tint (-2C); consolidate the
display producer rather than shipping conflicting loading and normal maps.
Use one shared mask/palette for generation and final map; never invent a thaw
to recover final geography. Changing the production producer requires matching
normal-map and generation acceptance, not a hidden visual-only replacement.

Use deterministic paths integrated from available wind/current pairs, seeded
from stable cell/source identity; rain intensity follows moisture/precipitation
with documented model units. Bound path/particle count and raster extent;
clock advancement changes presentation only. Start from the accepted limits:
16 wind paths, 18 current paths, 32 rain origins and at most 24 raster updates/s.
Resource pressure drops transient samples, never canonical data or readiness.
No independent timer per effect: one presentation clock stops while hidden,
and reduced motion shows stable phase endpoints without moving trajectories.

## Verification

Replay identical checkpoints/clocks in and out of order; compare deterministic
frames, final raster and producer immutability. Verify stage continuity, monotone
ice membership, pixel equality through Climate/Arrival, water-only currents,
wet-field rain and unavailable vector pairs. Test fast readiness, sparse/late
previews, wrong epoch, cancel/retry, errors, hidden/resume, reduced motion,
resize/device recreation and saturated upload slots. Ordinary desktop/Android
captures must show readable text, convincing motion and the same final map;
prototype/browser acceptance does not qualify those production paths.
