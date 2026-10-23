# Terrain-v2 frozen numerical reference contract

This note preserves the original Python/SciPy Terrain-v2 reference as model
revision 3. Its machine-readable contract and read-only comparator are in
[`tests/fixtures/terrain_v2/manifest.json`](../../../tests/fixtures/terrain_v2/manifest.json)
and [`script/terrain_reference.py`](../../../script/terrain_reference.py).
The six full NumPy world archives remain in the retained experiment bundle;
small raw fixtures keep representative stage, atlas, height, material and color
samples in this repository. The native implementation's separately versioned
revision-4 numerical contract is documented in
[`terrain_v2_revision_4/numerical-spec.md`](../../../tests/fixtures/terrain_v2_revision_4/numerical-spec.md).
That contract does not rewrite or relabel the revision-3 Python reference.

## Revision-5 extension

Revision 5 addresses the reported smooth terrain: a 16-block,
0.044-km final-column octave follows the unchanged revision-4 atlas and five
regional stages. See the [contract](earlydev_0_1_1_s1_refinement.md#revision-5-local-detail).
The source-map identifies base inputs; a new manifest binds the v5 oracle
and samples. Prior fixtures remain unchanged.

## Frozen identity

The mandatory seed order is `0, 1, 42, 777, 2004, 8800555`. Every seed uses
size 256, 12 tectonic epochs, 3 erosion steps, 24 provisional-climate
iterations, 80 final-climate iterations, currents enabled, wind sign `+1`,
and coastal elaboration enabled. The world contract is 65,536 blocks square,
1,024 blocks deep, a 256 by 256 atlas, block sea level 384, maximum surface
height 1,020, and geological sea level 0 km.

The frozen prototype source files and SHA-256 values are:

| File | SHA-256 |
| --- | --- |
| `common.py` | `23ff4496b6738d31e6fff63f52ac5acd4760b9b27ed398d7bfa5fb22435c7bd9` |
| `geology.py` | `aade3e716eb5c3805ebc88067472845bfd8d5db2ffb837d2a037de4d8aff9f36` |
| `coastal.py` | `fbcf5d1709d042d668bde49fa59dcf755dbee97e89fa75103b7be25c3c6fadee` |
| `hydrology.py` | `6d5f655ef1cfb3bae1f450a9c22883f8f9e56eba19390502d0130507598a47ab` |
| `climate.py` | `b5e1cc8188376d66ec231373e84a3dc6fa2a34c4a434d53d49ebafe4eee29a6f` |
| `refinement.py` | `180aa8f538fd41a9acae113b207167250d0debe15d243904be2a5fedcab21349` |
| `pipeline.py` | `6eac193b7b912ef30b5696dd4cffee37103762701b91850184f4bbf7679aa616` |

The source-map digest is
`5de753a4068c71d742f5ce7e75e4e8bcabbaad5fddaf6c8404dc41ba00d45704`. Its
preimage is UTF-8 `json.dumps(sources, sort_keys=True)` output, using Python's
default JSON separators and no trailing newline:

```json
{"climate.py": "b5e1cc8188376d66ec231373e84a3dc6fa2a34c4a434d53d49ebafe4eee29a6f", "coastal.py": "fbcf5d1709d042d668bde49fa59dcf755dbee97e89fa75103b7be25c3c6fadee", "common.py": "23ff4496b6738d31e6fff63f52ac5acd4760b9b27ed398d7bfa5fb22435c7bd9", "geology.py": "aade3e716eb5c3805ebc88067472845bfd8d5db2ffb837d2a037de4d8aff9f36", "hydrology.py": "6d5f655ef1cfb3bae1f450a9c22883f8f9e56eba19390502d0130507598a47ab", "pipeline.py": "6eac193b7b912ef30b5696dd4cffee37103762701b91850184f4bbf7679aa616", "refinement.py": "180aa8f538fd41a9acae113b207167250d0debe15d243904be2a5fedcab21349"}
```

That digest identifies source files only. The frozen manifest separately pins
the runtime: Python 3.12.14, NumPy 2.5.4, SciPy 1.18.1, matplotlib 3.11.2,
Pillow 13.0.0, psutil 7.2.2, and arm64. It also pins each seed's configuration,
identity file and complete `world.npz`, the qualification record, and the
render palette source `prototype/render_maps.py` at
`19c596dcc7b9cce5b91d7307fadc246e89e64e18a2b19729d87a3360e4df9a65`.
The reference manifest SHA-256 is
`d0e6ade7aed51a34a864c1dc497d3b3921d10204a8cc3602b71f7d53ab96db97`.

## Random stream and floating-point rules

The source uses NumPy `Generator(PCG64(seed))`; in the pinned NumPy runtime,
`default_rng(seed)` also creates PCG64. Stream identity includes both seed
arithmetic and the order and shape of API calls. `common.periodic_fbm_at`
masks its seed to 64 bits, then for each octave and each of three harmonics
draws `integers(-frequency, frequency + 1, size=2)`, replaces `(0,0)` with
`(frequency,0)`, and draws one scalar `uniform(0, 2*pi)` phase. It accumulates
cosine terms in loop order with amplitude `0.5**octave`, then divides by the
sum of amplitudes. Default settings are five octaves and start frequency two.
`periodic_fbm` samples at float64 coordinates and casts the completed field to
float32.

The generator uses independent streams at these source-defined offsets:

| Use | Seed |
| --- | --- |
| Plate/province and kinematic tectonics | `seed` |
| Crust warp, substrate, arc segmentation, relief detail | `seed + 211`, `+307`, `+401`, `+409`, `+521`, `+727` |
| Initial crust warp and province shape | `seed + 701`, `+709`, `+719` |
| Clustered-core switch, lobe regime, hotspot placement | `seed + 8321`, `+9421`, `+9991` |
| Coastal displacement fields | `seed + 811`, `+812`, `+821`, `+822` |
| Climate phase and periodic noise | `seed`, `seed + 1987` |

The seed-42 oracle in `rng_oracle_seed42.json` pins eight raw PCG64 words and a
separate sequential Generator stream through bounded integers, scalar and
array uniform draws, `random(4)`, and `vonmises(0.75, 1.8, size=3)`. Raw words
are numeric uint64 hex. `bits_le` values are little-endian serialized bytes;
for example `776b74c4780f0640` decodes to binary64 bits
`0x40060f78c4746b77`. The pinned source uses von Mises sampling for clustered
crust centers, so this vector protects a transform used by the model.

Preserve the source's explicit dtype boundaries. Grid coordinates, periodic
bilinear interpolation, climate calculations, and many intermediate
expressions use float64; geology, coast, hydrology and climate arrays are cast
to float32 at their explicit assignment/return points. Plate IDs are int16.
Periodic bilinear sampling converts coordinates to float64, wraps with
remainder, uses floor for lower indices, and evaluates the four weighted terms
in source order. Smoothing calls `scipy.ndimage.gaussian_filter` with
`mode="wrap"` and the pinned SciPy version. When checking revision 3, preserve
its cosine, sine, exponential, `tanh`, `hypot`, `expm1`, and square-root
operations, their order, and their array dtypes. Native revision 4 makes one
explicit, identity-bound exception: the geological shelf exponential receives
the already-rounded float32 exponent as a double input to `std::exp`, then
narrows the result to float32. The native configuration identity includes
`model-revision=4` and `shelf-exp=exp64-to-f32-v1`; all other operations retain
their source-defined order and dtype boundaries. Do not reassociate expressions
or enable fast-math in either revision.

Surface height uses `floor(block_sea_level + vertical_scale * (elevation -
sea_level))`, then clips to `[1, 1020]` and casts to int32. Water height follows
the same floor rule and clips to `[0, 1023]`. Python's built-in `round` in
hydrology uses ties-to-even. Material selection uses submerged status,
temperature `<= -2` °C, and precipitation `> 0.20`; it does not interpolate
palette colors. The exact seven RGBA8 palette entries and their source identity
are in the manifest.

## Fixtures and comparison

Each seed retains 65 raw fields: fixed-coordinate pre-coast geology,
coastal elevation, initial drainage, provisional climate, post-erosion
drainage, final climate, final atlas diagnostics, surface height/materials,
and final RGBA pixels. Atlas samples use x and y indices `0,16,…,240`; surface
samples use block coordinates `128 + 4096*k` for `k=0…15`. Arrays are C-order
row-major `[y,x]`. The manifest gives every field's dtype, shape, coordinate
set, unit, file and SHA-256. Total raw fixtures are about 318 KB; complete
world archives are retained by hash in the external bundle rather than copied
here.

The CLI only verifies and compares. It has no baseline-generation or
auto-update operation. A candidate directory contains `candidate.json` and
the corresponding raw files. The manifest must use schema
`minecraftclone.terrain-candidate.v1`, bind the frozen manifest/source hashes
and world configuration, keep the six-seed order and per-seed config hashes,
and list all 65 fields in canonical order with the frozen descriptors and
candidate file SHA-256 values. Compare with:

```sh
python3 script/terrain_reference.py compare \
  --candidate /path/to/candidate \
  --bundle /Users/core-just/.codex/reports/minecraft-worldgen-2026-10-19/bundle
```

Every input hash and descriptor is checked before comparison. Raw values are
compared byte-for-byte; reports include seed, field, `[row,column]`, expected
and actual raw hex, and decoded values. RGBA reports identify pixel coordinate,
changed channels, and both colors. Configuration, source, schema, sample-grid,
palette, runtime, or fixture drift is an error. Verify only the retained source
and full-world archive identities with:

```sh
python3 script/terrain_reference.py verify-bundle \
  --bundle /Users/core-just/.codex/reports/minecraft-worldgen-2026-10-19/bundle
```

Changing the baseline requires an explicit new versioned reference contract;
editing a fixture or manifest in place is rejected by the comparator.
