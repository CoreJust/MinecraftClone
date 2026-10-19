# S1 — Proposed generation and production integration

Part of the [S1 planning draft](earlydev_0_1_1_s1.md); proposals below are not
owner-approved implementation choices or runtime evidence.

## Proposed terrain design

Use an explicit world seed and generator revision, with a validated immutable
configuration. The current native cosine-wave height kernel has no seed input;
the new contract must propagate seed/configuration consistently to every
authority and generation consumer. Identical seed, revision, configuration and
coordinates must regenerate identical results regardless of chunk request
order, worker count or eviction. The precision/quantization contract must make
authoritative heights/materials consistent across supported platforms; it is
an implementation decision to specify and test, not a floating-point assumption.

The owner's later clarification supersedes the earlier noise-led composition:
tectonic simulation or a credible approximation generates the base heights;
the seed decides the arrangement and scale of landforms. The following is a
proposed architecture, with the actual model selected after comparison in
[the tectonics research/design note](earlydev_0_1_1_s1_tectonics.md).

1. **Seeded geology:** establish plates, continental/oceanic crust structure
   and motion. Do not equate one plate with one continent: the result must
   support varied landmasses and oceanic portions within a plate model.
2. **Tectonic relief:** model or approximate collision, subduction, spreading
   and their regional effects. Generate related mountain belts/foothills,
   deep basins/trenches, ridges and island/coast structures from that geology.
   Merely naming a random range mask “tectonics” does not satisfy the goal.
3. **Optional elaboration:** bounded relaxation/erosion or local noise may
   refine slopes and coastlines where needed. Preserve the base geography;
   an exponential local-noise term is an available detail technique, not a
   requirement to recreate the continents or mountain placement independently.
4. **Height:** map base and elaborated elevation into block heights with the
   declared quantization rule, preserving bedrock, standing clearance and
   relative basin/range structure. Avoid broad clamping that erases that relief.
5. **Sea level:** choose S deterministically per seed/configuration using a
   bounded world-wide sample, then reuse that one value for all chunks.
   A proposed initial quantitative interpretation of “roughly half” is
   45–55% submerged columns, measured on a separately declared validation grid.
   Keep the result and sample definition; do not normalize each chunk independently.

Use a bounded coarse world model/atlas plus deterministic local refinement,
subject to measured cost and feature preservation. Plate preprocessing belongs
to world creation, not every chunk request or gameplay tick. Bound atlas
resolution, iterations, memory and initialization time; expose progress and
cancellation. Cache immutable outputs by seed, configuration and generator
identity, with query work independent of player travel history.

Respect the existing 65,536-block X/Y period in plate ownership, motion,
neighborhoods and height queries. Tile edges/corners and both wrap seams must
agree; adopting a spherical-paper algorithm does not authorize changing world
topology. Do not choose a new dependency before checking its build/license
contract. Plate model, resolution, evolution/detail parameters, seed corpus
and proposed land-area tolerance need evidence before becoming constants.

## Integration with the actual game

The current code exposes only Air/Stone in `Chunk.hpp`. `TerrainGenerator`
produces height tiles and materialized chunks; `HeightTileSurfaceMesher` and
the normal `PlayerClient` render height-derived quads. Existing streamed
messages carry heights, not the new material layers. A block-enum change alone
cannot deliver S1.

The implementation must establish one terrain sample contract for height,
latitude, sea level and material-at-Z, then make these agree:

- Generated chunks and authoritative collision, including lazy regeneration.
- Streamed surface data/configuration and versioned codec validation.
- Top and side rendering: cliffs expose the correct four ground layers,
  underlying stone and bottom bedrock where visible; LOD does not paint an
  entire cliff with its top material or erase biome/material boundaries.
- Generation refinement: preview and final content agree at published sample
  points, and stale work cannot install a different seed/configuration/revision.
- Spawn: choose a bounded deterministic safe land position with standing
  clearance instead of retaining a fixed Z that may lie inside the new terrain.

Whether to transmit compact material metadata or derive it from an authenticated
shared configuration is an implementation design choice. Arbitrary client-side
guesses are not acceptable. Existing generation-stage, CoreLang configuration,
streaming, cancellation and LOD contracts remain constraints. S1 need not add
full voxel streaming for unedited heightfield terrain; S2 must explicitly
design edit representation, replication, meshing and collision updates before
claiming interactive editing works in the ordinary world.
