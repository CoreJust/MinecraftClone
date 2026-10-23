# S1 — Coastal, island and world-scale diversity

The owner's examples describe diversity goals, not Earth silhouettes: smooth,
curved irregular and ragged coasts; elongated islands with satellites; close
large-island groups; medium/small archipelagos; remote islands/clusters;
subcontinental islands; uneven world-wide land concentration and sparse unusual
terrain. Evaluate the corpus and actual crops, not only component sizes. No
world must contain the same landmark catalog.

## Seeded continental material

Initialize 3–5 large, 6–10 middle and 10–18 small anisotropic crust provinces.
Large-core radii are 0.17–0.225L, scaled by `sqrt(5/large_count)`; middle radii
0.065–0.11L and small radii 0.025–0.052L. Vary aspect ratios up to 2.4, 2.7
and 3.2 respectively, conserving each ellipse's area while changing elongation.
Warp material coordinates by 0.040L periodic detail. Core values take the
strongest influence; suppress close overlaps with oceanic material rather than
summing everything into a percolating supercontinent or drawing uniform moats.

Seeded regimes vary placement. Separated large cores use bounded farthest
candidate placement; a 30% clustered regime instead permits uneven large-core
placement. Middle/small material samples a periodic von-Mises concentration
lobe with seeded strength 0.35–0.85; 35% of middle and 55% of small placements
may attach near ancestral large cores. This creates related satellites as well
as remote material, rather than uniformly scattered identical islands.

Within the clustered regime, an independently seeded 35% case adds a broad
0.55–0.70 cosine lobe to **initial continental material**. It supplies strong
half-world asymmetry without painting late height noise over tectonics.
Median-centre and logistic-transform material, then transport it through the
same plate history. These are procedural initial conditions, not a theory
predicting Earth's continental distribution. Seeded supercontinents are valid.

## Coast regimes and island mechanisms

Initial inherited roughness is stronger in cold/disrupted margins. During
plate evolution, OO convergence receives a spatially segmented crest term
instead of a uniform ridge. Unequal crests/saddles can produce related island
groups. Preserve arc uplift separately from total uplift.

After the single sea offset, elaborate the inherited coastline with a bounded
periodic coordinate warp, restricted to ±1.2 geological km of sea level.
Displacement scale is `70 + 650*activity + 1400*cold²` world units, multiplied
by a smooth nearshore window. Two RMS-normalized periodic vector fields combine
long and short wavelengths. Activity derives from uplift/trench/rift fields;
cold is a smooth synthetic paleoclimate proxy. Passive warm coasts remain
smoother; active margins become irregular and cold margins dissect into inlets,
peninsulas and offshore remnants. Inland relief outside the band is unchanged.

This warp is an explicit fracture/paleoglacial **geometry approximation**, not
a glacier, sediment or wave solver. Actual rainfall/cold-catchment valley
carving then runs in [hydrology](earlydev_0_1_1_s1_hydrology.md). Material choice
uses final [climate](earlydev_0_1_1_s1_climate.md), not this proxy. There is no
fixed polar material boundary. Separate sediment deposition, sea-level history
and detailed glacier dynamics were considered but are not claimed as implemented.

Seed 0–3 eligible oceanic hotspot tracks. Each contains eight age-decaying
elliptical deposits with peak coefficient 2.5 km and `exp(-age/3.5)` subsidence;
offsets follow final local plate velocity. This can form isolated islands,
clusters or submarine seamounts. It approximates a history rather than actually
reconstructing time-varying plate paths. Preserve locations and contribution
fields. Sparse tracks, rifts, straits, drowned basins and interactions with
existing relief provide unusual structures without repeated per-seed landmarks.
Active volcano gameplay, an exhaustive rare-landform catalog and copied Earth
templates are outside S1.

## Evidence and limits

The [experiment](earlydev_0_1_1_s1_experiments.md) retains the failed uniform
topology, intermediate weak-asymmetry version and selected model. It records
island areas/aspect ratios, groups/remoteness, coastal complexity at two scales,
half-domain land concentration, actual crops and CPU/memory costs. The half-world
statistic scans periodic axis-aligned windows; it is not spherical latitude.
Morphological scores supplement visual review and do not prove geological realism.

[NOAA](https://oceanservice.noaa.gov/education/tutorial_estuaries/est04_geology.html)
supports distinct flooded, tectonic and glacial coastal mechanisms;
[USGS](https://www.usgs.gov/geology-and-ecology-of-national-parks/geology-glacier-national-park)
supports glacial valley/lake forms; [USGS hotspots](https://www.usgs.gov/news/volcano-watch-hotspots)
supports intraplate tracks as distinct from boundary belts. None validates our
warp, coefficients or event frequencies. Primary-source access limits and
unselected fuller mechanisms remain in the retained research note.
