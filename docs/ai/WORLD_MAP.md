# CPU world-map exporter

`mc_world_map` runs the production CPU Terrain-v2 atlas producer without
starting the game, renderer, network stack, or CoreLang. It uses the same
`GenerationConfiguration` and `prepareAtlas` implementation as gameplay.
Atlas height, temperature, and water images are diagnostic views of numeric
atlas fields. A requested `top-material` image comes from the canonical
regional sampler, which follows the same ancestor chain used for gameplay.

The output path must not already exist, and its parent directory must exist.
The exporter accepts atlas resolutions 256 and 512, at most six seeds, and
limits total output to 512 MiB. It reports invalid options and I/O failures on
stderr. A failed multi-seed run retains completed seed directories and a
`manifest.json` that records the failed status and completed seeds.

```sh
mc_world_map \
  --seed 42 \
  --atlas-size 256 \
  --layers height,temperature,water \
  --output /tmp/terrain-seed-42
```

Use `--seed-list 0,1,42,777,2004,8800555` instead of `--seed` to export the
owner-supplied seeds in that order. `--seed` and `--seed-list` are mutually
exclusive. Layer names are `height`, `temperature`, `water`, and
`top-material`; layer names cannot repeat. Request `top-material` with a
regional extent:

```sh
mc_world_map \
  --seed 42 \
  --atlas-size 256 \
  --layers height,top-material \
  --region 0,0,64 \
  --output /tmp/terrain-seed-42-region
```

`--region X,Y,EXTENT` identifies a canonical square in wrapped world-block
coordinates. `EXTENT` must be one of 16384, 4096, 1024, 256, or 64 blocks, and
`X,Y` must align to it. Each region export samples a 65×65 grid through
`TerrainWorld::prepareRegion` and `sampleRegion`; the selected extent and
origin are recorded in the seed metadata.

The output contains an ordered root manifest and one directory per seed:

```text
output/
  manifest.json
  seed-42/
    metadata.json
    height.ppm
    temperature.ppm
    water.ppm
    top-material.ppm
    fields/
      height.f32le
      temperature.f32le
      water.f32le
      ocean-mask.u8
      lake-mask.u8
      river-mask.u8
      top-material.u8
      top-height.u16le
```

Only requested layers are written. PPM files use binary P6 RGB pixels. Atlas
fields are flattened row-major as `[y,x]`; raw floats and 16-bit heights are
little-endian IEEE-754 float32 and unsigned 16-bit values. Raw files include
their units and byte order in `metadata.json`. Height and water values are
geological kilometres; temperature is degrees Celsius, and top heights are
block Z coordinates. Water masks are one byte per atlas cell. Top-material IDs
use the shared block IDs: Air 0, Stone 1, Grass 2,
Dirt 3, Bedrock 4, Snow 5, and Ice 6. The top-material PPM uses that same ID
array and the RGB palette listed in its metadata.

Each seed's `metadata.json` records every configuration field, canonical
configuration identity, the frozen Terrain-v2 source-map SHA-256, atlas
dimensions and layout, regional origin and extent when requested, raw-field
descriptions, source-field ranges, check results, and memory use. Timing
separates atlas simulation, canonical regional preparation and queries, pixel
rasterization, and PPM/raw-field serialization. Serialization timing includes
PPM and raw-field writes but excludes the metadata file itself.
