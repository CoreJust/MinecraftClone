# Native Terrain-v2 atlas generation

`mc_generation` owns the deterministic CPU global-atlas producer and its
configuration contract. It has no renderer, window, network-startup, or
CoreLang initialization dependency. Canonical regions, surface columns and
chunks are consumers owned by later tasks.

The public `generation/Generation.hpp` API exposes the complete configuration
identity, immutable stage snapshots, progress, cancellation and failure
results. Configuration is validated before work begins; progress reports only
completed work. Consumers may coalesce snapshots but must not fabricate an
atlas-complete result.

`tests/generation/` binds the native seed-42 geology, coast, hydrology and
climate stages to the frozen raw reference fields, and checks configuration,
progress/cancellation and numerical helper contracts. The immutable fixture
manifests and Python comparators are documented here because they define the
expected values; they never rewrite goldens. Revision 3 preserves the original
Python/SciPy prototype. Revision 4 is the approved portable global-base
identity (`shelf-exp=exp64-to-f32-v1`); revision 5 keeps that base and
separately adds final-column local detail. See the
[versioned numerical contract](../ai/decisions/earlydev_0_1_1_s1_numerics.md)
and the [revision-3](../../tests/fixtures/terrain_v2/manifest.json) and
[revision-4](../../tests/fixtures/terrain_v2_revision_4/manifest.json)
fixture manifests.

For the independent CPU target, configure `src/generation` with
`MC_GENERATION_BUILD_TESTS=ON`, build `mc_generation_tests`, and run that target's
CTest cases. Parent-project tests register the same seven native generation
sources in `mc_tests`. These checks establish numerical component behavior;
they do not establish full gameplay, renderer, GPU, or release acceptance.
