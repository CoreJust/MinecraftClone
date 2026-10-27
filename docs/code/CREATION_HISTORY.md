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
