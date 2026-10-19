# S1 — Climate and material algorithm

Part of the [generation design](earlydev_0_1_1_s1_generation.md). Climate-driven
materials in S1 are an explicit owner decision and replace the former fixed
outer-20% polar bands. This bounded procedural climate is neither a weather
forecast nor a spherical atmosphere simulation.

## Coupled preparation

After tectonics and connected-ocean drainage, run 24 provisional climate
iterations. Their rainfall/cold catchments drive bounded fluvial/glacial
shaping. Recompute water bodies, run 80 final climate iterations, and reroute
final discharge from final rainfall. Do not iterate to an unbounded fixed
point. Record the last-step residuals; the output is a fixed simulation budget,
not a claim of equilibrium. Final climate changes discharge without another
erosion cycle.

All climate heights use geological kilometres before block-height compression.
Synthetic insolation is `I=(1-cos(2πy/L))/2`: a warm central belt and cold
wrapped seam, continuous on this world's existing XY torus. Set
`T_base=-18+48*I-6.5*max(elevation-sea,0)` °C. The 6.5 °C/km lapse is an
explicit approximation. Climate, coasts and height can move material
boundaries away from any fixed Y fraction.

## Wind, currents and transport

Prescribe predominantly zonal winds with smooth periodic perturbations and
pressure-gradient deflection. Smoothed base temperature supplies the pressure
proxy. Scale mean transport with grid resolution and iteration count so the
run spans comparable world distances at 256² and 512².

Smooth wind curl into an ocean streamfunction. Set vertices touching dry cells
to zero; currents are its rotated face gradient. This gives divergence-free
face flux and impermeable shores by construction. Normalize maximum current
transport to `0.30*N/iterations` cells/iteration. Lakes evaporate, but do not
carry ocean currents. This is a wind-driven circulation sketch, with no deep
ocean, Coriolis dynamics, seasonal forcing or sea-ice feedback.

Advect ocean heat and atmospheric moisture with conservative first-order
upwind face flux. Substep until the outgoing Courant sum is at most 0.65.
Ocean temperature relaxes toward insolation with coefficient 0.02 per nominal
80-iteration run step. Coastal land receives the smoothed *transported heat
anomaly*, weighted by ocean proximity; an arbitrary coastal warmth offset is
not used. Current-off experiments must remove this transport effect.

Initialize moisture to 1 over water and 0.24 over land. Remove a bounded
fraction for background rain and positive wind-facing elevation rise;
replenish water cells toward saturation through evaporation. Integrate removed
moisture into precipitation. Record initial/final moisture, evaporation,
rainfall and their budget residual. Precipitation is normalized model flux,
not millimetres/year. Reversing wind must move windward/lee rainfall; test this
on a controlled ridge rather than inferring it from a colourful map.

## Materials and future consumers

The canonical column sampler bilinearly samples final climate and uses:

| Condition | Land top / ground |
|---|---|
| Temperature ≤ −2 °C | Snow / Ice |
| Warmer, integrated precipitation > 0.20 | Grass / Dirt |
| Warmer, drier | Dirt / Dirt |

The dry Dirt top is a proposed material interpretation within the existing
S1 palette. It introduces no sand or new biome content. Cold ground is Ice,
including the permitted shallow-seabed band. No snow cap is placed on
submerged columns. All cases keep the [exact soil/bedrock rules](earlydev_0_1_1_s1.md).
Snow and Ice are full solid blocks; melting, thin snow and friction are not
implied. The separate snow/climate-zone diagnostic may use moisture too;
the canonical material table above controls actual voxels.

Store temperature, precipitation/moisture, winds, currents and zones for
future decorations, flora/fauna and seasons. S1 adds those fields and their
material effects, not the future systems themselves. Global maps depict
water-body data; S1 voxels above the bed remain Air until S4.

[NOAA](https://www.noaa.gov/education/resource-collections/ocean-coasts/ocean-currents)
supports wind-driven current/heat transport, and
[NWS](https://www.weather.gov/hfo/climate_summary) supports elevation and
windward/lee climate mechanisms. They do not validate these coefficients or
this synthetic circulation. See [measured causal checks](earlydev_0_1_1_s1_experiments.md).
