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

Treat the owner's formula as the shape of the composition, not a mandate to
exponentiate an unbounded raw value. A proposed construction is:

1. **Continents:** low-frequency, multi-octave periodic noise supplies broad
   landmasses, ocean basins and smooth regional elevation.
2. **Range grouping:** a spatially coherent ridge/range field controls where
   local relief is strong. Broad transition bands reduce its amplitude through
   lower mountains and foothills into plains. A smaller isolated-peak component
   permits exceptions without making the whole landscape a field of spikes.
3. **Local relief:** normalized multi-octave local noise is exponentially
   shaped with a finite exponent and amplitude. The range field modulates its
   strength; fine detail cannot dominate the continental structure.
4. **Height:** combine continental elevation and local relief, quantize using
   the declared rule, and bound the result within the vertical world while
   preserving bedrock and enough ceiling clearance for a standing player.
   Tune amplitudes before relying on clamping, which can create flat plateaus.
5. **Sea level:** choose S deterministically per seed/configuration using a
   bounded world-wide sample, then reuse that one value for all chunks.
   A proposed initial quantitative interpretation of “roughly half” is
   45–55% submerged columns, measured on a separately declared validation grid.
   Keep the result and sample definition; do not normalize each chunk independently.

Evaluate fields in global wrapped coordinates with an exact 65,536-block
horizontal period. Tile edges, corners and the world wrap seam must agree in
height and material; visible noise seams or abrupt terrain steps are failures.
Do not choose a new dependency without checking the existing build contract.
Frequency, octave count, amplitudes, range widths, seed corpus and the proposed
land-area tolerance need a preview/calibration pass before their final values
become acceptance constants.

This composition is a design proposal, not evidence that realistic geography
has been achieved. NVIDIA's [procedural-terrain chapter](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu)
describes combining noise at different scales and spatial distortion; the
range-control field and acceptance criteria here are our project proposal.

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
