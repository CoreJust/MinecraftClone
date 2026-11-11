# V10 river-bed correction

The f775 foundation uses the separately prepared corrective bed field
`;river=c8f9b851f7891e555165648d473b39df1ecffc9b73ae65cf5b14ed9f5ba04703`.
`TerrainWorld` validates that identity for seed 42 and also accepts its exact
`;fine=e52e2e82b3e9873e14e543e0a7583fb978a182f1555997bda6febcd9ff25061a`
variant. LocalGame and Server select the fine variant. The benchmark can select
either variant. The owner selected the c8f9b851 river and e52e2e82 relief
pair for the preview. Build acceptance remains open.

## Fine-field composition

For foundation `F`, corrected bed `C`, fine L2/L3 height `T`, standing stage
`S` and corridor support `r`, the base composition is `B = C + a * (T - F)`,
where `a = smooth(F - S) * channel_clearance * (1 - r)` and `smooth` is clamped
cubic smoothstep. Clearance is zero in nominal courses. Outside them it rises
from zero at `1.25*w` to one at `2.5*w` for the nearest indexed course of
halfwidth `w`, or is one when no indexed course is near. Zero attenuation
preserves `C` exactly.

The provider applies a separate signed inland relief term to `B` only where
the water query at `B` classifies the point as dry. It scales the term by `a`,
the minimum contact weight, and `smooth((B - S - 4 m) / 12 m)`. Contact weight
is zero within a protected contact radius and feathers over the next 8 m
using periodic distance. The guard also rejects negative relief inside a
containing course. Water and climate queries use the resulting bed with
canonical course provenance. With `use_v10_fine_terrain`, canonical elevation,
surface, map, grid and column queries use this L3 composition.

The native L2/L3 field is bound to the f775 parent, seed 42 and period 65,536 m.
The v2 Python oracle SHA-256
`6365ed7e79067c12fbc4d717e17ce2e1a239fbb672abf170ecf9cf4b1c255717`
records historical kernel provenance. The earlier e37 corrective field is also
historical. The current field's 256×256 sites use nine parent probes. The native
receipt records NumPy PCG64 draws and the immutable feature
plan hash. L2 adds parent-conditioned hills/swales, C2 support, a signed coast
profile and reduced plain texture; L3 adds exposed rock and nested gullies. The
fine field changes elevation. Point parity and offscreen fixtures cover sampled
locations and views.

Each standalone `V10FineTerrain::bed` call samples all nine points in its parent
ring. The provider already computes and validates foundation F at periodically
wrapped XY, then passes that center into the fine field, which samples the same
eight neighbors in order. When attenuation is nonzero, this changes the provider
path from ten parent evaluations to nine without changing the fine calculation
order. Zero attenuation still skips the fine field; preparation remains at
591,296 parent queries.

`queryV10NativeBedSamples` preserves order and exact periodic XY and returns the
native foundation, standing stage and owner; owner zero is valid. Preparation
arrays are immutable. The prepared field drives canonical surface queries and
curved rendered beds, including positive corrections and original reach/course
provenance. Native endpoint stages preserve standing-water precedence and
actual-bed depth checks; there is no hydraulic solve. River-map contact
refinement uses `sampleV10WaterGeometry` for configured bed and exact water
membership without climate. Full annual samples remain for river temperature
and contact cold classification and palette values. Receiver extensions skip
unused endpoints; contact refinement caches exact XY geometry and reuses full samples.
Already-dry coarse contact polygons are discarded before annual palette queries.
Stored courses provide metric viewport coverage without queries during panning.
The final S2 map composites refined contact patches with standing-water and
river coverage. These are map overlays. S2 changes the bed geometry, while
physical water and floating ice belong to S3.

`exportRiverConstraints` retains one-metre subdivision, heights, lineage and
ordering. The S2 spatial index selects course/image pairs before clipping;
synthetic and actual-atlas comparisons must match the original exporter.

`RiverBedMesh` triangulates constraints in crop-local coordinates with pinned
CDT, preserving its licenses. Its incidence tolerance is 64 binary64 epsilons
times crop extent. Height callbacks receive original world XY, including added
vertices. Failures propagate rather than producing an unconstrained fallback.

Course-free rectangular fans may use checked direct triangulation. Admission
requires no clipped course or tangent anchor and continuous radial height/material
state. It preserves canonical sampling and every center/perimeter constraint;
ambiguous or discontinuous fans retain CDT and the same Q5/bridge checks.

L0–L2 fans retain their semantic sectors, authority boundaries and transition
heights. Renderer XY uses Q5 metric triangles; heights are evaluated at the final
representable positions. Planar duplicate-node quotients preserve course chains,
sector ownership, area and exterior attributes. Distinct heights at shared XY
remain distinct. Collinear bridge faces remain submitted: curvature can give
them area, so their pre-curvature zero normal requires a finite upward fallback.
Texture UVs include the fractional Q5 origin to keep shared-edge texture phase.

Normal scene tests reject reversed final triangles and broken authority seams.
The explicit diagnostic census inspects every fan and deliberately stops without
publishing a scene, even with zero failures; its nonzero exit is not a production
failure. CPU geometry, native rendering, morphology, performance and owner
acceptance are separate gates. A passing mesh does not establish natural terrain
or acceptable river-bank shapes.

Android's optional `river_diagnostic=first_failure` launch extra writes one failed
fan to app-internal `river-first-failure.*` files. It preserves exact double bits,
PSLG inputs/output and off-grid deltas for host replay. Successful quotient dumps
are suppressed. Capture failures preserve the original geometry rejection; a
missing or incomplete dump is not qualification evidence. Normal launches do not
enable capture.

Renderer Q5 repairs preserve recorded constraint paths. Off-grid intersections
route through existing nearest lattice nodes with one checked retriangulation.
Exact-grid intersections may add shared render vertices only after independent
integer predicates attest proper crossings of retained constraints. New heights
use the same corrected river-bed sampler, then the existing sector correction and
world clamp. No input vertex moves. Exact areas follow declared routed boundaries. Ordered
radial-owner paths survive collapsed geometry; quotienting requires equal emitted
height and material at both endpoints. Subpath, ownership, area and exterior
guards remain mandatory; unattested vertices and off-grid crossings fail closed.
If a rounded crossing node is absent, routing may use an incident endpoint
strictly within one Q5 unit; ordered paths and post-routing crossing checks
remain mandatory. This changes declared paths without moving input vertices.
