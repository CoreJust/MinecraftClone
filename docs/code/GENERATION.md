# Frozen Terrain reference fixtures

This module owns only the immutable reference trees under `tests/fixtures/`.
The generation implementation and its C++ consumer tests are documented with
the producer tasks; these files define the numerical identities those
producers must match.

- `terrain_v2/` preserves the original Python/SciPy revision-3 oracle and its
  fixed source/configuration hashes. Its read-only comparator reports raw
  stage, surface, material, and RGBA mismatches with coordinates and values.
- `terrain_v2_revision_4/` is the separately versioned approved portable
  global model. Its identity requires
  `shelf-exp=exp64-to-f32-v1`; it has 36/36 raw-field parity across Mac,
  Windows, and Android. Manifest SHA-256:
  `9a2644bbe81a88876bac4f84f334f8c0a5d3525295e83122dcce10f92f02d6eb`.
- `terrain_v2_revision_5/` retains revision 4 and adds the final-column local
  detail oracle. It has 8,418 regional/final/materialized point checks and
  393,216 precipitation-bit checks across Mac, Windows MSVC, and Android
  arm64, with zero mismatches. Manifest SHA-256:
  `593da126bfebecc56f2412424aba50a160105a3471d2b5c58f2c1146108294f2`.

The v4 global-base receipt and v5 local-detail receipt are separate contracts;
v5 does not replace v4. The Python tools verify the manifests and compare
native candidate exports. Verification and comparison never update expected values. The explicit revision-5
`record` command creates a new fixture tree and refuses an existing output path;
changing expected values requires a newly versioned, reviewed contract.
