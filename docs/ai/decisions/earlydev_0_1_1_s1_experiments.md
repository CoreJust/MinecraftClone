# S1 — Selected prototype and measured budgets

Terrain-v2 adopts **prototype v3, 256²** for the fast-start global atlas;
512² is a distinct
higher-detail configuration. Research, actual Python experiments and source-level
production design are complete; game implementation remains later. All source,
raw arrays, maps, identities, rejected versions and replay instructions are in
the retained bundle at
`/Users/core-just/.codex/reports/minecraft-worldgen-2026-10-19/bundle/README.md`.
The retained v1/v2/v3 experiment labels and PDF predate the Terrain-v2 name;
they are research iterations, not the Terrain-v1/Terrain-v2 design versions.
The explanatory 16-page PDF, actual seed 42 stage captures, six additional
parameter variants and figure sources are retained separately at
`/Users/core-just/.codex/reports/minecraft-worldgen-2026-10-19/report/`.
The stage-capture rerun matches every frozen final atlas array exactly.
[Selection rationale](earlydev_0_1_1_s1_rationale.md) explains alternatives,
ordering and parameter sensitivity without claiming an optimized physical model.
The PDF predates the added loading-map, 0.1.5, platform-performance and shared
2D testing requirements; current S1 contract notes supersede those older limits.

## Measured results

Host: current Mac, arm64, 12 logical CPUs; Python 3.12.14, NumPy 2.5.4,
SciPy 1.18.1, one numerical thread. Each row is one fresh process, not a
cold-disk or repeated-trial latency guarantee. Numerical generation excludes
validation, local benchmarks, compression and PNG rendering.

| Ordered seed | Generation s | Peak process MiB | Land % | Land in richest half % | Fjord / inlet candidate cells |
|---|---:|---:|---:|---:|---:|
| 0 | 1.599 | 101.7 | 50.0 | 59.4 | 50 / 59 |
| 1 | 1.554 | 101.9 | 49.9 | 87.0 | 32 / 23 |
| 42 | 1.512 | 103.0 | 50.0 | 67.7 | 25 / 70 |
| 777 | 1.612 | 101.7 | 50.4 | 86.1 | 15 / 6 |
| 2004 | 1.569 | 101.8 | 50.1 | 64.7 | 9 / 41 |
| 8800555 | 1.524 | 101.6 | 49.6 | 61.6 | 43 / 60 |

Global retained arrays occupy 5.06 MiB; process RSS also includes the Python
runtime, libraries, diagnostics, scratch and serialization. Fresh process wall
through validation/benchmarks/compression was 2.21–2.32 s; PNGs are separate.
Seed 42 stages: tectonics 573 ms, coastline 15, initial drainage 117,
provisional climate 111, shaping 436, final climate 149, final drainage 109.

Seed 42 at 512² took **8.154 s / 212.8 MiB**, with 20.25 MiB atlas arrays.
Aligned land/sea agreement with 256² was 98.53%; height MAE 0.118 geological km.
Resolution is part of world identity, not a silent runtime quality change.
Local 16³ chunks took P50 4.54–4.73 ms, P95 4.66–4.88 ms in Python
(12 locations/seed, including canonical column sampling). One 65×65 regional
query took 22.0–23.3 ms at the five extents. These are bounded-function costs,
not native scheduling or gameplay FPS evidence.

## Morphology and causal qualification

Inspected all six world/diagnostic maps, coastal/island crops, regional detail
and ablations. The gallery shows smooth reaches, irregular promontories,
ragged margins, elongated islands/satellites, island groups, remote islands and
large islands. One selected remote island is 12,733 blocks from substantial
land; one cluster window contains 14 separate 2–1200-cell components. The
half-world statistic scans periodic axis-aligned windows. Scores supplement
visual inspection; they do not prove Earth realism.

All 35 focused tests and 14 generated world variants passed their invariants.
Tests cover seam/order/regeneration, exact layers, DAG/outlets, lake beds,
coastal contrasts, climate transport/rainshadow and shared sampler/chunk parity.
Repeated seed 42 arrays match exactly on this runtime. A supplementary untuned
seed 314159 took 1.609 s/101.8 MiB and passed the same checks.

Controlled seed 42 changes demonstrate actual component effects:

- Zero plate motion: mean absolute final-height change 1.473 geological km.
- No terrain erosion: maximum bed difference 0.240 km.
- No currents: maximum coastal temperature difference 0.984 °C.
- Reversed wind: mean absolute precipitation change 1.627 model-flux units.
- Coastal elaboration off: retained same-source full/crop comparison.

Earlier results remain visible for comparison:

| Attempt | 256² / 512² model s | Reason replaced |
|---|---|---|
| Plate probes | 0.335 hard / 0.503 smoothed, geology only | Hard ownership made polygon cliffs; images retained. |
| v1 | 1.41–1.52 / 6.60 | Every seed had one dominant connected landmass; overly smooth coasts. |
| v2 | 1.49–1.61 / 7.70 | Better island sizes, weak coast contrast and only 54.5–67.5% half-world concentration. |
| v3 selected | 1.51–1.61 / 8.15 | Added explicit coastal regimes and seeded strong concentration. |

## Production bounds and limits

Measured host thresholds, declared before final qualification, were <5 s and
<128 MiB process RSS at 256²; all passed. The later owner
[loading/performance contract](earlydev_0_1_1_s1_performance.md) supersedes the
provisional 5 s desktop/10 s Android preparation targets; the 4 s desktop
initial load includes more than atlas computation. Reserve 16 MiB for default
pinned atlas, 96 MiB build scratch and 32 MiB regional retention, separately from
in-flight/preview/chunk budgets; explicit 512² mode reserves 32 MiB atlas and
192 MiB scratch. These are admission defaults to verify on the native port,
not inferred Android results. Capacity failure never silently changes terrain.

The model uses kinematic plates, procedural crust/coastal proxies, synthetic
climate and spill-filled lakes. It omits sediment conservation, realistic dry
basin balance, full ice/ocean/atmosphere dynamics and calibrated geology.
D8 rivers and bilinear coarse fields can leave directional/faceted detail;
retain native visual checks. Fjord/inlet masks are geometric candidates.
Native canonical arithmetic, cross-platform reproduction, actual materials/LOD,
ordinary gameplay and inherited defect fixes remain implementation gates.
