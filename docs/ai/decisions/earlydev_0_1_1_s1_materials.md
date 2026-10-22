# S1 — Terrain-v2 solid materials and exact columns

Indexing convention: `H(x,y)` is the Z coordinate of the highest
solid block, and `S = SEA_LEVEL` is the highest Z that S3 may fill with water.
Thus `H >= S` is land, and `H < S` is submerged terrain. This convention must
be shared by generation, rendering, collision and S3 water placement.

At `z=0`, always generate Bedrock. At `z>H`, generate Air in S1. For the
remaining solid blocks (`1 <= z <= H`):

| Column | Layer | Block |
|---|---|---|
| Land: `H >= S` | `z == H` | TOPMOST_BLOCK |
| Land | `H-4 <= z < H` | GROUND |
| Land | `z < H-4` | Stone |
| Submerged: `H < S` | `S-4 <= z <= H` | GROUND |
| Submerged | `z < S-4` | Stone |

The bedrock rule takes precedence if a configured shallow column would overlap
the bottom. TOPMOST_BLOCK/GROUND follow the [climate material table](earlydev_0_1_1_s1_climate.md):
Snow/Ice when cold; Grass/Dirt when warmer and moist; Dirt/Dirt when
warmer and dry. Submerged ground can therefore be Ice; no grass/snow cap is
generated below sea level. Inland lake beds above S retain the land rule.

Boundary examples, assuming enough depth above bedrock:

| Highest solid block | Result immediately below/at sea level |
|---|---|
| `H=S` | Topmost at S, four ground blocks at S-1 through S-4. |
| `H=S-1` | Four ground blocks at S-1 through S-4; Air at S until S3. |
| `H=S-3` | Two ground blocks at S-3 and S-4. |
| `H=S-4` | One ground block at S-4. |
| `H=S-5` | Stone-only column above bedrock. |

The owner's shallow/deep-water rule uses a fixed sea-level band, not four
ground blocks beneath every seabed.

Climate uses smooth toroidal insolation, elevation, ocean heat transport and
wind/moisture effects. There is no fixed Y-band material predicate. Snow and
Ice remain full solid blocks; thin snow, melting and friction are not inferred.
