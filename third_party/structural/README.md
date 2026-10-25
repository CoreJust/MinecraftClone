# Frozen v9 structural kernel

This directory retains the approved Dendry/structural C++ call closure from the
`coherent-practical-macro-watershed-weather-v9` source snapshot. `SOURCE_MANIFEST.json`
records each original file hash and local file hash. NoiseLib is GPL-3.0; its
license is retained in `LICENSE`.

The kernel equations are unchanged. Local changes add checked in-memory span
constructors to `CenteredControl`, `RegisteredWorldFields`, and `SeededWorldSurface`.
The memo cache also stops retaining misses when its logical budget is full and
returns caller-owned values for deterministic recomputation. Shared ownership
keeps nested graph dependencies alive during concurrent queries. The existing
file constructors remain part of the frozen wrapper source.

Compile the five NoiseLib source files with the pinned Clang contraction mode
(`-ffp-contract=on`). The owning `V9Terrain.cpp` enables that mode around the
inline kernel definitions and restores strict mode for preparation code.
Contraction changes can alter branch decisions in the generated hierarchy;
strict mode is not numerically interchangeable with the frozen kernel.

Registered inputs are a 65,536 m torus with 256-square layers: control base and
slope at cell centres; structural macro at cell centres, zero lowpass at
vertices, and slope at cell centres. Query results are source metres. Raw
structural H0 includes D3; global v9 canonical terrain uses the separate
macro-only/shelf/valley bed. Regional water geometry belongs to the later
water-conditioned watershed provider.
