# S1 — Selected geological model

This is the algorithm contract for later implementation, qualified by the
[temporary experiment](earlydev_0_1_1_s1_experiments.md). It is a kinematic,
physically motivated approximation. It does not solve mantle convection,
force balance, isostasy or an Earth reconstruction. Parameters are procedural
art-scale choices; geological kilometres are separate from compressed block Z.

## Initial state and transport

Use a periodic square atlas, row Y/column X, with samples at `i*65536/N`.
Select 16–22 periodic Voronoi sites from the seed, independent velocities
250–850 world units/epoch and independent continental material. Continentality
`C∈[0,1]` comes from independently seeded large, middle and small elongated
crust provinces with attached satellites and overlap-only oceanic margins.
A seeded concentrated-material regime allows strongly uneven half-world land
coverage; dispersed worlds retain separated cores. Median-centre this material
field and apply a logistic transform. The [diversity contract](earlydev_0_1_1_s1_diversity.md)
fixes the recipe and its coastal/island elaboration. A plate may contain both oceanic and continental material;
neither plate count nor plate IDs prescribe continent count.

Initial geological elevation is `-4.2 + 5*C + 0.18*substrate` km. The substrate
is declared residual noise, not evidence of tectonics. Warp the periodic plate
coordinates with three-octave detail of amplitude `0.030*world_extent` to bend
boundaries. Keep that warp fixed throughout the history.

Run 12 epochs. Blend plate velocities continuously using weights
`exp(-(distance²-nearest_distance²)/(2*b²))`, `b=0.055*world_extent`.
Backward semi-Lagrangian bilinear sampling transports continentality and
accumulated relief. Smooth continentality by 0.45 atlas cells; retain 88% of
advected relief each epoch. Move sites by their velocities and wrap them.
Record initial/final continental material mean: this stable transport is not
area conserving, and material drift must remain visible in the evidence.

## Relief from motion

Signed divergence of the continuous velocity is the areal-strain proxy.
Normalize it by blend width, cell spacing and an 800-unit speed reference;
clip convergence/divergence strengths to `[0,1.2]`. Smooth forcing at broad
`0.016L` and narrow `0.004L` widths, with a half-cell minimum. Thus atlas
resolution changes sampling, rather than changing belt widths in world units.

Smoothstep continentality from 0.38–0.62 into continental and oceanic weights.
Its broad neighborhood supplies continuous CC, OC and OO boundary weights.
Use broad convergence for continental collision/arc uplift, narrow convergence
for oceanic arcs and trenches, and divergence for spreading ridges or
continental rifts. Seeded plate rank selects OO polarity; smooth its contact
transition so nearest-pair changes cannot create a relief jump.

Per epoch, accumulate 0.24 times these art-scale coefficients:

| Effect | Coefficient in geological km |
|---|---:|
| Continental collision | +2.5 |
| Continental arc | +2.2 |
| Oceanic island arc | +2.0, segmented crest |
| Trench | −1.5 |
| Ocean spreading ridge | +0.9 |
| Continental rift | −0.55 |
| Broad rift shoulders | +0.55 |

Cap accumulated uplift at 6 km, subduction at −3 km, divergence relief at
−1.5…2 km. Final elevation adds those fields to transported crust elevation;
at most 0.30 km of five-octave ruggedness is modulated by uplift. Segmented
oceanic arcs and sparse age-decaying hotspot tracks add island/seamount
potential, with separate provenance. Mountains
therefore follow motion-derived belts before detail is applied. Transform
boundaries receive no independent broad mountain term; divergence is an
imperfect stress proxy and may misrepresent oblique/transform contacts.

Choose the single global sea offset from the median tectonic elevation, then
hold it through coastal elaboration and erosion. This gives approximately half submerged terrain
without per-region normalization. Preserve plate/crust/motion, initial/base
elevation, uplift, trench and divergence layers for inspection and future
volcanic provenance. Volcano gameplay is outside S1.

## Research and rejected design

[Cortial et al.](https://perso.liris.cnrs.fr/apeytavi/website/publication/hal-02136820/)
supports coarse crust simulation followed by amplified terrain; its author
abstract was inspected, not an available complete implementation. The chosen
velocity blend, coefficients and torus adaptation are our experiment.

The first nearest-plate transport probe produced polygon-shaped cliffs and
was rejected. Its image and source/configuration identity remain in the
experiment bundle. The revised model smooths motion and forcing rather than
covering those discontinuities with independent terrain noise.
