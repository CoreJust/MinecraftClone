# Planetary creation fields

[`GenerationMapHistory`](../../src/client/include/client/GenerationMapHistory.hpp)
prepares an immutable CPU illustration from final elevation, water levels and
plate IDs on a periodic square grid. Preparation copies input spans; caller
storage may then be released. Coordinates use source cells and elevations use
metres. Normalized illustration time is separate from world preparation and
Start readiness.

Each supplied solid core translates and rotates through direct inverse sampling.
Preparation reconstructs its vacated connected land footprint from surrounding
bathymetry and water levels. The returning body restores that footprint through
geometric overlap. Scalar height-offset events permit temporary bridges, rifts
and submergence. Quintic timing settles motion and offsets before the final
weather interval. At the endpoint, elevation and water levels equal the supplied
final fields.

`evaluate()` writes reusable elevation, water, thermal and periodic-coordinate
arrays. Cold core interiors retain rigid sampling; local joins reconstruct a
complete scalar surface. Invalid time leaves the previous frame intact. Separate
frames can evaluate the same immutable plan concurrently. Float coordinates
wrap again after conversion so rounding cannot index beyond the periodic grid.

The [focused tests](../../tests/client/generation_map_history_tests.cpp) cover
finite fields, endpoint identity, scalar land merging/splitting, hotter plate
boundaries, vacated-island removal, rigid rotation and periodic seam coordinates.

Desktop binding and the complete requested creation sequence remain pending.
The one-island diagnostic establishes transport and continuity only. Computational
join heat is diagnostic data; the visible material treatment still requires
comparison with the owner's planetary-scale direction. Map/model identity,
climate, rendering, readiness and platform qualification remain their owning
components' contracts.

Preparation rejects floating inputs whose magnitude exceeds `max(float)/24`.
The denominator is the maximum 16 scalar events plus eight units of headroom:
height stays within 17 input magnitudes, and water-depth intermediates within
20, below the float output limit. This is a numeric domain, not terrain
rescaling; all supported evaluator arithmetic is unchanged.
