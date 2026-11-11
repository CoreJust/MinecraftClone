# V10 annual-mean surface cover

`classifyV10AnnualMeanSurfaceCover` selects material from the eight-phase mean;
it does not change source temperature, biome IDs, inventories or geometry.
Canonical columns, the native world map and generation history share this rule.

Dry land uses Snow when mean snow-water equivalent (SWE) is at least 0.1 m,
or when SWE is positive and mean temperature is below 0 C. Snow takes priority
over glacier ice; otherwise glacier thickness at least 0.1 m selects glacier ice.
Ocean sea ice retains its 0.1 m threshold. Physical lakes and rivers gain no cap;
S2 remains bed-only. Wet inland/deep beds use Dirt. Adjacent ocean beds below MSL
and within the existing 20 m band use Sand, including canonical block queries.
Ocean provenance is separate from adjacency and travels in protocol version 27.

This surface-coating policy differs from the model's permanent-snow biome,
which uses the minimum stock across physical phases. It adds no seasonal clock
or snow volume. Owner visual acceptance remains separate.

Snow, glacier and sea-ice map colors replace the underlying terrain hue. Neutral
height and contour shading remains; scalar climate overlays keep their legends.
Creation and world maps use the same opaque material palette.

Maps separately project inland ice where the actual inland-water footprint has
eight finite seasonal forcing samples with a negative annual mean. This is an
annual cartographic depiction, not ice thickness or a new physical stock. Display
kind 4 stays separate from physical cover IDs. Both map views share the helper;
warm water remains open. Block materialization is unchanged until S3.

Actual-chunk presentation uses a wrapped disk derived from the existing 40-chunk
radius and Euclidean near-first ordering. Authority retains its source/neighbor
coverage. The visual center travels independently of coarse reuse; capture
qualification checks complete, unique disk membership. Edit merges preserve it.
