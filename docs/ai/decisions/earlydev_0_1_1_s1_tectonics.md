# S1 — Tectonic terrain foundation

Planning/research note, 19 October 2026. Part of the [S1 design](earlydev_0_1_1_s1.md).
The owner wants seeded tectonic simulation or a credible approximation to
produce base heights. Different-sized continents, mountains, trenches, islands
and peninsulas should emerge naturally; additional elaboration is conditional.
This supersedes the earlier continental-noise plus independent range-mask design.

## Model candidates and evidence

This is a bounded primary-source survey, not an implementation selection or
benchmark. Sources were checked on 19 October 2026. No prototype was run.

| Candidate | Evidence and relevant limit | What the project must establish |
|---|---|---|
| Simplified evolving 2D plates, PlaTec-style | [PlaTec](https://sourceforge.net/projects/platec/) and [Viitanen's 2012 thesis](https://www.theseus.fi/handle/10024/40422) describe a simplified plate-motion model with continental collision, subduction and evolving terrain. Its initial height/crust field includes fractal noise; it is not full geophysical simulation. | Whether evolution produces the required seed diversity and plausible relief on a bounded coarse grid; feature preservation under local refinement. |
| Procedural approximation of tectonic effects | Cortial et al., *Procedural Tectonic Planets* (2019), describe approximating plate collision/subduction, evolving crust and amplifying the resulting large-scale relief. Verified against the [author's abstract](https://perso.liris.cnrs.fr/eric.galin/2019-2015.html) and [publication listing](https://perso.liris.cnrs.fr/eric.galin/articles.html); [paper](https://hal.science/hal-02136820/document). | Automatic seeded initial conditions, believable continent/coast diversity and adaptation to this game's periodic 2D topology. The paper's planetary model is not a drop-in proof for this world. |

The categories overlap: both may approximate physics and evolve over steps.
Do not reject an evolving model merely because it uses noise for initial
conditions. The question is whether tectonic state and its consequences
generate the geography, rather than optional cosmetic noise supplying it all.

The directly inspected [Mindwerks C++ fork](https://github.com/Mindwerks/plate-tectonics/blob/main/src/platecapi.cpp)
accepts seed and map dimensions. Its [rectangle indexing](https://github.com/Mindwerks/plate-tectonics/blob/main/src/rectangle.cpp)
wraps both horizontal axes, while [initialization](https://github.com/Mindwerks/plate-tectonics/blob/main/src/lithosphere.cpp)
uses dense dimension-sized fields. This supports investigating a bounded coarse
atlas; it does not establish practical cost at 65,536 squared or chunk streaming.
Seed input alone does not establish cross-version/platform bitwise determinism.
Do not adopt the fork yet: archive/fork license labels differ and exact reuse
terms/provenance need verification before any dependency or copied code.

The publisher endpoint returned HTTP 403; the author-hosted abstract/listing
were accessible. The paper's full algorithm and its implementation/reuse
conditions were not reviewed in this pass. No source establishes this game's
initialization time, memory, frame performance or guaranteed landforms per seed.

## Proposed comparison and acceptance

Use a deterministic coarse periodic world model, followed by bounded local
queries/refinement, as the first architecture to evaluate. Do not simulate a
dense 65,536-square plate field or repeat global evolution for each chunk.
The [generation proposal](earlydev_0_1_1_s1_generation.md) owns identity,
cancellation/progress, bounded resources, height scaling and material integration.

Compare candidate outputs with optional post-tectonic detail disabled, then
enabled. Initial stochastic crust is allowed. Show initial/final plate and
crust maps, motion, relevant boundary/history fields, base elevations, final
heights and materials. Retain seed/configuration and rejected results.

Use the [USGS plate-motion account](https://pubs.usgs.gov/gip/dynamic/understanding.html)
as a qualitative check: continental convergence can build mountain belts;
subduction relates trenches to nearby ranges/arcs; divergence relates to
spreading/rifts and ocean ridges. Those relationships are a plausibility
reference, not a demand to reproduce every Earth process. Do not assume every
mountain must follow a present-day boundary; a model may retain earlier relief.

Across a predeclared seed corpus, inspect connected ranges with foothills,
different continent sizes, deep ocean features, islands and peninsulas.
Proposed same-class checks include island arcs and ridges/rifts. Each seed may
produce a different mix; do not hardcode a landform count or require every
example in every world. Check both wrap seams, land-area statistics and height
compression into 1,024 blocks without erasing trenches or flattening peaks.

Measure atlas resolution/iterations, creation time, peak memory, query/refinement
cost and regenerated-output agreement. Set acceptance budgets before prototype
qualification; no invented timings or library suitability claims. Select the
model from morphology and those costs, then specify any erosion/detail needed.
