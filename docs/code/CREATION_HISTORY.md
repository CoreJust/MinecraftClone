# Planetary creation fields

[`GenerationMapHistory`](../../src/client/include/client/GenerationMapHistory.hpp)
prepares immutable CPU history fields from final elevation, water levels and
plate IDs on a periodic square grid. It copies input spans; caller storage may
then be released. Coordinates use source cells and elevations use metres.
Normalized illustration time is separate from model generation and Start readiness.

Each moving body supplies its actual binary source footprint, not a radius-based
ownership region. Masks contain exactly resolution-squared cells, are nonempty
and mutually disjoint, and must leave some background bathymetry. Support collars
are positive and at most one quarter of the source side. The unpublished circle
API is replaced; consumers provide the actual masks.

Preparation reconstructs vacated height and water jointly over the footprint
union. Canonical footprint ordering stabilizes floating reductions. Translations
and rotations directly sample each body; normalized scalar contributions combine
overlaps without selecting a height winner. Each weight is support times coverage
divided by the greater of one and total coverage; background keeps the remaining
weight. Every overlapping body influences height and water. Water travels with its bed and stage.
Scalar offset events permit bridges, rifts and submergence. Motion/events settle
smoothly into exact final elevation, water and periodic coordinates.

`evaluate()` fills reusable scalar, thermal, overlap and provenance arrays.
Unique cold interiors retain rigid source sampling. `source_body_id` is diagnostic:
a renderer must not threshold it to choose terrain height or color. Material
callbacks may use `final_coordinates` only with the continuous
`source_mapping_weight`; overlap or ambiguous torus mappings reduce that weight
toward zero. The remaining contribution must come from the continuous scalar
state, without an independent-world RGB fade. Computational remelt heat is not
an approved visual plate-border treatment.

Invalid time leaves the prior frame intact. Separate frames may evaluate one
immutable plan concurrently. Both double sampling and float output coordinates
wrap below the periodic upper bound, including negative sub-ULP remainders.

At most 64 bodies and 16 events are accepted. All floating inputs have magnitude
at most `max(float)/24`, derived from the event limit plus eight units of headroom.
Unsupported values are rejected, never rescaled or saturated. Scalar height stays
within 17 input magnitudes and water-depth intermediates within 20.

The focused CPU proof covers 22 unit cases and the actual three-major-body fixture:
six source orders, eight times and eleven channels agree; all 9,801 endpoint-owned
cells and final fields are exact. Independent double sampling bounds cold-interior
height error. UBSan and libc++ bounds checks passed; the ASan startup deadlock
provides no qualification. Prior circle-overlap and order failures remain evidence.

Complete canonical story rendering, PlayerClient/native integration, visual
approval and platform/performance acceptance remain parent task 0483 and
integration task 0621 work. Kernel timings are not native FPS evidence.

## Typed illustration consumer

`GenerationMapAnimator::updateNative` accepts live snapshots before an atlas exists.
The screenshot remains until a valid source arrives. Platec publishes copied,
throttled fields; canonical H0 and terrain replace them through scalar interpolation.
Cell centres and vertices retain their respective coordinates.

The animator retains one visible clock through these replacements and late atlas
arrival. Geometry, thermal grain and emission share the source field. The native
consumer currently keeps geography stationary: fixed-region drift and hydrology
rewind were removed after owner feedback. Moving and deforming plate ownership
requires a coherent material mapping before drift can return. Material grain
wraps across both map seams.
Climate and water wait for real fields without changing authoritative readiness.
Water heads rise from basin bottoms toward final stages. Wind/current effects
begin after the ice age; rain accompanies water formation. Highlands cool through
dark red into crust while contacts remain incandescent. One opaque ice field
advances then retreats to its persistent present-day stock floor. Reduced motion
selects the final source. Complete visual acceptance remains open.

Creation and world maps call the same `appendS2MapWeatherTriangles` renderer.
Continuous trails sample bilinear wind/current vectors; current segments stop
at the coast. Deterministic stratified seeds and age offsets bound density and
motion without grid-snapped arrowheads. Rain uses fixed periodic impact anchors
with short local fall and splash events; precipitation controls their frequency.
Both views use normalized geometry and elapsed seconds. Rain sizes stay bounded while anchor density follows projected map scale;
scissor-limited iteration skips offscreen impacts before field sampling. Plate boundaries draw all outlines
before foreground strokes so adjoining corners do not erase each other. Wind has faint broken
ribbons beneath the directional marks. The overlay continues at readiness;
the current trail style still needs visual acceptance.

The final image matches the canonical palette pixel-for-pixel; weather is separate.
CPU captures do not qualify native startup or playback. Start requires completed
scene/prompt frames and fresh input, independently of illustration completion.

Cooling potential follows displayed elevation and contacts through their 900 ms
adoption; seeded pools and animated grain break equal-height fronts. Cumulative
exposure only cools. Hot-only boiling fades through the configurable eight-second
window. Early local crust and its retained incandescent fissures use this same
thermal age; neither requires a separate region or a reset when relief arrives. Late uplift cannot reheat rock. River-only map coverage follows the same filling clock even when the uncarved
bed has zero depth; physical water fields stay unchanged. Rain follows cooling; water filling and ice
advance slow by one third. Ice-age deserts expand, then return to modern biomes.
`thermalSample()` exposes cell height/potential/progress for CPU diagnostics only.

Local material temperature now fades incandescence through dark red before grain-based crust nucleation. Highland/continental rank leads cooling; basin depth spreads ocean cooling, while hot contacts retain light. These are illustration fields and do not alter the final model.

ClientAudio follows the visible creation phase with immutable sound clips.
Material cues use accepted, deduplicated edits; progress chips require active
targeting. Grounded steps read materialized support only. Focus/exit changes
stop recurrence; missing devices never block gameplay.
Recorded CC0 rain, boiling and ice beds use overlapping contiguous segments;
Stone and Dirt have distinct recorded cues. Accepted breaking crumbles last
600 ms; other material gestures keep their own durations. Packaged PCM music loads before
audio starts through desktop installed assets or Android's asset manager.
Ready and focused gameplay each repeat their music bed with source overlap.
Map state edges request open/close cues. Fresh accepted airborne-to-supported
player state requests a material landing cue; it suppresses a simultaneous step.
Asset/mixing checks do not establish acoustic quality or native playback.

The native thermal budget starts when the client first adopts geographic relief,
not when the first raw Platec snapshot appears. Before that event, actual source
fields and incandescent radiance continue evolving; irreversible exposure remains
bounded before relief arrives. A local crust can appear around hot fissures as that
exposure grows. The existing exposure is retained when H0 arrives and
continues over the configured eight-second budget, including its scalar adoption.
Water, rain and the water-phase boundary wait for that completion and the short
pause. Physical generation readiness is independent of this illustration clock.
Late client delivery can therefore extend the visible sequence; it is not proof
that the producer itself generated H0 late. Ordinary cold/warm receive schedules
are separate from the optimistic retained-field replay and from runtime timing.
