# S1 — Terrain-v2 generation design

Status: **finalized by the owner on 19 October 2026 as Terrain-v2**, for
EarlyDev 0.1.1 S1. Terrain-v1 is the currently implemented repeating-wave
generator. Terrain-v2 is the settled replacement design; production
implementation and native acceptance remain planned.

Use a bounded, physically motivated
procedural model: **global simulation → canonical regional elaboration → local
chunks**. Do not allocate a 65,536² simulation or repeat global preparation for
chunk requests. Model details, constants, measured results and implementation
ownership are linked below.

The experiment's v1/v2/v3 labels describe research iterations, not terrain
versions. Terrain-v2 adopts the selected **prototype v3 at 256²**, with 512²
as a separate higher-detail world configuration. The retained source identity,
maps and PDF remain the evidence for that choice. The native port defines its
own explicit numerical/configuration revision within Terrain-v2; the name
alone is not a sufficient world or save identity.

## Global dependency graph

```mermaid
flowchart LR
    A[Seeded crust and moving plates] --> C0[Coastline elaboration]
    C0 --> B[Initial connected-ocean drainage]
    B --> C[Provisional climate: 24 iterations]
    C --> D[Bounded erosion and glacial shaping]
    D --> E[Final climate: 80 iterations]
    E --> F[Final discharge and water bodies]
    F --> G[Validated immutable atlas]
    G --> H[First-touch regional hierarchy]
    H --> I[Canonical surface columns and 16³ chunks]
```

1. [Geology](earlydev_0_1_1_s1_geology.md): seed independent plate and crust
   fields; transport crust through 12 moving-plate epochs. Continuous velocity
   strain produces collision belts, trenches, ridges/rifts and island arcs.
   Residual detail follows geology. Plate identity is not continent identity.
   [Coast/island diversity](earlydev_0_1_1_s1_diversity.md) adds uneven crust
   provinces, segmented arcs and sparse hotspot histories.
2. [Terrain and water](earlydev_0_1_1_s1_hydrology.md): choose one sea offset,
   identify periodic connected seas, derive an acyclic drainage graph and lake
   spill surfaces, shape rainfall-fed valleys and cold coastal inlets, reroute.
   Keep beds distinct from water. Diagnose landmasses and sheltered inlets.
3. [Climate](earlydev_0_1_1_s1_climate.md): synthetic toroidal insolation,
   elevation cooling, prescribed winds, wind-driven ocean heat transport and
   moisture/orographic rain. These fields select S1 materials and provide
   future ecological/seasonal inputs. The provisional pass resolves the
   climate–erosion dependency with a fixed budget.

The global atlas is the intended future unloaded-world-map detail: continent
shape, coasts, islands, ranges/trenches, seas/lakes and large rivers. It does
not represent individual blocks or future decorations. Geographic features
emerge from the seed/model; do not paste identical landmark counts into each
world. The test/demo map colours water data despite S1's dry game basins.

## Regional and local generation

[Regional elaboration](earlydev_0_1_1_s1_refinement.md) uses the existing
16,384→4,096→1,024→256→64 extents, deterministic lattice values, parent
constraints and bounded halos. Request order, eviction and worker scheduling
cannot change the terrain. The prototype evaluates the canonical field
without a cache; production must actually consume typed parent artifacts.

Local sampling applies the shared H/S convention, climate material table,
exact four-block soil/seabed band and bottom Bedrock. A single sampler feeds
materialization, authority, surface previews and detailed map export. Preserve
65,536-block XY wrapping and bounded 1,024-block Z; distinguish geological
height from compressed block height. Safe spawn follows atlas readiness.

## Selection and implementation boundary

[Choice rationale](earlydev_0_1_1_s1_rationale.md) defends alternatives, stage
dependencies and parameter defaults, separating measurements from procedural
calibration and explicit simplifications.

[Experiments and budgets](earlydev_0_1_1_s1_experiments.md) record all six owner
seeds `[0,1,42,777,2004,8800555]`, source/configuration identities, maps,
performance, component ablations, rejected morphology and approximation limits.
Retain the selected default resolution and bounded preparation budgets. Finite
iterations, progress and cancellation are mandatory. No busy preparation runs
on the gameplay tick.

[Ownership/scheduling](earlydev_0_1_1_s1_architecture.md) defines immutable
world identity, typed atlas/regional/surface results, dependency leases,
near-first priority and independent memory budgets. The
[implementation sequence](earlydev_0_1_1_s1_implementation.md) names exact
production files, current bypasses, protocol/renderer changes and native tests.
The initial [tectonics survey](earlydev_0_1_1_s1_tectonics.md) retains alternatives
and source-access limits; it is superseded for algorithm selection.

This design removes the model-selection prerequisite for implementation.
Python measurements establish the measured host/model costs and appearance;
they do not establish C++/Android costs, gameplay FPS, multiplayer correctness
or cross-platform bitwise regeneration. Those gates remain explicit native
implementation work, alongside the inherited defects and ordinary S1 gameplay
acceptance. No in-game map UI, physical water or later biome content is added
by this planning/experiment task.
