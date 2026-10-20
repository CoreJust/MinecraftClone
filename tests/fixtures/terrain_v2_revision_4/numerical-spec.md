# Terrain-v2 model revision 4 numerical contract

Revision 4 retains Terrain-v2's geometry, model coefficients, stage order,
random streams, and float32 assignment boundaries. It changes only the shelf
exponential in `Geology.cpp`: the exponent is evaluated and rounded as
`float`, passed to `std::exp(double)`, then narrowed to `float`:

```cpp
float const shelf_exponent = -0.5F * continental_offset * continental_offset;
float const shelf = static_cast<float>(
    std::exp(static_cast<double>(shelf_exponent))
);
```

The build must disable floating-point contraction and reassociation for the
generator (`-ffp-contract=off` on the tested AppleClang build and `/fp:strict`
on the tested MSVC build). Keep the exponent's float operations and the cast
back to float in this order. The configuration identity binds the change with
`model-revision=4` and `shelf-exp=exp64-to-f32-v1`; revision 3 and revision 4
identities cannot compare equal. The Python source-map digest remains the
same because it still identifies the unchanged seven-file prototype source
map, not the native numerical implementation.

The checked-in `manifest.json` and its 61 little-endian sample fields were
captured from the owner-approved isolated double-exp candidate. It covers the
Tectonics geology-elevation snapshot for seed 42, six final atlas fields for
each of the six reference seeds, and four canonical surface fields for each
seed. Atlas samples use indices `0,16,…,240` in C-order `[y,x]`; surface
samples use world blocks `128 + 4096*k`. The 61 checked-in raw sample files
total 39,424 bytes. The manifest SHA-256 is
`9a2644bbe81a88876bac4f84f334f8c0a5d3525295e83122dcce10f92f02d6eb`. It also
binds SHA-256 digests for all 36 complete final atlas arrays; those 5,898,240
raw bytes are retained in the external
approved-candidate archive named by the manifest. The archive preserves the
candidate's original output metadata as generated; those metadata strings
predate the model-revision-4 identity change, while the numerical operation
and output bytes are the approved revision-4 behavior.

The prior Python reference remains unchanged at
[`../terrain_v2/manifest.json`](../terrain_v2/manifest.json), SHA-256
`d0e6ade7aed51a34a864c1dc497d3b3921d10204a8cc3602b71f7d53ab96db97`.
Its raw files continue to describe the original NumPy/SciPy outputs. The new
reference is additive and separately versioned; it does not rewrite or
relabel those bytes.

On the recorded AppleClang 17.0.0.17000603 and MSVC 19.44.35229.0 builds, the
approved candidate produced identical raw final atlas bytes for six seeds at
resolution 256: 1,179,648 float values and 18 mask arrays. The 24 canonical
surface sample files also match the prior frozen fixture at those sampled
coordinates. This is measured evidence for those builds, inputs, and seeds;
it is not a claim that arbitrary system `libm` implementations return
identical results for every input or platform. Android parity remains a
separate platform check.

Verify the checked-in sample manifest and files with:

```sh
python3 script/terrain_revision_4_reference.py verify
```

Compare a complete `mc_world_map` raw export using:

```sh
python3 script/terrain_revision_4_reference.py compare \
  --candidate /path/to/complete-export
```

When the retained full-field archive is available, the comparator checks raw
bytes and reports the first differing atlas coordinate. Without it, it
checks each full-field SHA-256 against the versioned manifest.
