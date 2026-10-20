# S1 — Why this model and these defaults

The owner requests a defended choice, not only an algorithm catalog. Select
the bounded v3 hybrid for native implementation: moving crust establishes
related relief, constrained shaping adds coasts and drainage, climate supplies
materials, and canonical regional detail avoids a full-world voxel simulation.
The [experiment](earlydev_0_1_1_s1_experiments.md) supplies the measurements;
the explanatory PDF and its stage/sensitivity evidence are retained at
`/Users/core-just/.codex/reports/minecraft-worldgen-2026-10-19/report/`.

## Alternatives and order

Octave noise remains useful residual detail. Alone it encodes neither moving
crust nor related trenches, mountain belts, river outlets or rain shadows.
The first hard-plate probe was actually run and rejected for polygon cliffs.
Smooth motion and forcing address that discontinuity before adding detail.
A fuller plate/geodynamic simulator is a possible future comparison, but was
not benchmarked here; no speed or fidelity superiority over PlaTec is claimed.
The selected model provides controllable, finite work without assuming an
unreviewed dependency, license or physical solver is required for S1.

Plate ownership and continental material are independent because mixed crust
must cross plate boundaries and one plate must not imply one continent.
Seeded, multiscale anisotropic provinces supply varied initial conditions;
transport and interaction then relate them. The failed all-megacontinent and
weak-coast versions justify overlap suppression and explicit coastal regimes.
The nearshore warp deliberately approximates fracture/paleoglacial geometry;
it is not disguised as simulated sediment or glacier dynamics.

Geology precedes ocean connectivity and climate because heights/coasts define
transport barriers and catchments. Climate also influences erosion, so a
24-iteration provisional pass supplies rain/coldness, three shaping passes
alter beds, and an 80-iteration final pass uses the resulting geography.
Final rainfall updates discharge without another erosion cycle. This breaks
the feedback loop with bounded work; it does not claim a converged equilibrium.
Global drainage precedes regional detail so neighboring regions inherit the
same outlets, lakes and channels instead of inventing incompatible watersheds.

## Parameter reasoning and evidence

| Default | Reason and limit |
|---|---|
| 256² atlas | Six required seeds took 1.512–1.612 s and 101.6–103.0 MiB process RSS; 512² took 8.154 s/212.8 MiB. The latter improves sampling but is a separate identity, not a silent quality switch. |
| 16–22 plates; 12 epochs | Multiple broad motion sources and finite history. In seed 42, 6/24 epochs changed mean final height by 0.931/1.353 geological km relative to 12. These are different histories, not convergence errors; 12 is a tested art/budget choice. |
| Three erosion passes | Reroute after each bounded cut while keeping shaping subordinate to broad geology. One/six passes changed global mean height by only 0.000184/0.000186 km; sparse channel effects make global means insufficient. The no-erosion maximum difference was 0.240 km. |
| 80 final climate iterations | Fixed transport duration with stability substeps. At 40/160, mean temperature changed 0.00105/0.000525 °C, but precipitation changed 0.1644/0.0972 model units. This supports bounded temperature stability for this seed, not calibrated rainfall or an optimum. |
| One median sea offset | Directly implements roughly half submerged terrain. Holding it through shaping preserves causal coast changes and consistent regional sea level. |
| World-scaled belts/coasts | Widths stay in world units when atlas sampling changes. Nearshore restriction protects inland relief; cold/activity modulation provides contrasted coasts. Coefficients and event frequencies remain procedural calibration. |
| Five existing regional extents | Reuse the established hierarchy while replacing ignored tokens with actual typed parent dependencies. Canonical lattice samples give order/eviction independence. |
| −2 °C / 0.20 material thresholds | Explicit full-block Snow/Ice and Grass/Dirt transition choices within S1's palette. Climate-driven selection is required; these are Terrain-v2 gameplay thresholds, not scientific biome boundaries. |

Geological elevation, block-height compression, relief caps, coastal displacement
and hotspot frequencies are documented bounded art-scale controls. They were
chosen to produce the inspected corpus without exceeding the declared host
budget; the experiments do not uniquely identify them. Do not present every
constant as physically measured. Native acceptance may require tuning followed
by a new generator revision and regenerated corpus.

## Architecture and reconsideration

One immutable atlas avoids repeated global simulation. Shared canonical
sampling removes the existing server/refinement and SparseWorld bypasses.
A CPU-only `mc_generation` library and `mc_world_map` tool make diagnostics
available without game/graphics startup; the current executable linkage cannot
provide that independence. Typed parent leases and separate byte budgets
preserve data lifetime and bound retained work under eviction/cancellation.

Revisit the model if native preparation, memory, gameplay or rendered terrain
fails acceptance, or if actual sediment balance, glacier history or calibrated
planetary climate becomes required. Python evidence removes model selection
as a prerequisite; native numerical identity and platform acceptance remain
implementation work. The PDF distinguishes measured results, mechanisms and
deliberate simplifications throughout.
