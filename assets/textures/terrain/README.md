# Terrain texture provenance

These unmodified PNGs come from Mojang Studios' official Minecraft Java Edition
1.21.1 client, released 2024-08-08. They supply the small block set rendered by
the native terrain path. The full client JAR is not stored in this repository.

The official version manifest is
<https://piston-meta.mojang.com/mc/game/version_manifest_v2.json> (downloaded
SHA-256 `ac37f92737df1a9d7660047136fedd3edebd90bd6e552f53ddc3f222079bc8fa`).
Its 1.21.1 entry points to
<https://piston-meta.mojang.com/v1/packages/499680d659b7a0ccb2fa9a809c345c5baf2ace7d/1.21.1.json>,
whose expected SHA-1 is `499680d659b7a0ccb2fa9a809c345c5baf2ace7d` and whose
downloaded SHA-256 is
`8047141a4d8de5ce23cc8390df56373b3a6c10f063d47b7bf3ea64e8c84666e9`. The
metadata identifies the client at
<https://piston-data.mojang.com/v1/objects/30c73b1c5da787909b2f73340419fdf13b9def88/client.jar>:
26,836,906 bytes, SHA-1
`30c73b1c5da787909b2f73340419fdf13b9def88`, SHA-256
`499f6897d1837516680f3114072d8106e11c9adcd933fe5cf051b551089b0c99`.
The release announcement is
<https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-1>.

| Repository file | Client JAR member | SHA-256 |
| --- | --- | --- |
| `vanilla-1.21.1/block/stone.png` | `assets/minecraft/textures/block/stone.png` | `33ac70ba8cb701087642a12c5fd29c1ba3006dc4730e9b014ed3a19ce7cdda7b` |
| `vanilla-1.21.1/block/grass_block_top.png` | `assets/minecraft/textures/block/grass_block_top.png` | `52f44b64ce884a13dfe35e4b4da2e35898b790dd4d7a5ea536f1b0c0d56d29b4` |
| `vanilla-1.21.1/block/grass_block_side.png` | `assets/minecraft/textures/block/grass_block_side.png` | `ab572ac7ebd373abdeb961de1f3cba0afcd91e1acb700745ece4f719790bae3d` |
| `vanilla-1.21.1/block/grass_block_side_overlay.png` | `assets/minecraft/textures/block/grass_block_side_overlay.png` | `43d98318407d9d122b83468cf183d18722e548c2144f9c2ffe2021b5c3e770e0` |
| `vanilla-1.21.1/block/dirt.png` | `assets/minecraft/textures/block/dirt.png` | `1f0f0e619e7dfe683ea45b9254859f14b0ed5f1debdb242ea878828575be5bf0` |
| `vanilla-1.21.1/block/bedrock.png` | `assets/minecraft/textures/block/bedrock.png` | `52f57443f329e53404c73c697ffd75770f1e2abf935a28315621fb0ca94b7571` |
| `vanilla-1.21.1/block/snow.png` | `assets/minecraft/textures/block/snow.png` | `05201107fc2c60db4dc59f778a9ca7cb6101b2a332da696b33ce4b0cd2a02019` |
| `vanilla-1.21.1/block/ice.png` | `assets/minecraft/textures/block/ice.png` | `4d158c24f2ec563d52806642ac7f4e20a5bda168af8c2ae528ed8e9ef1a58771` |
| `vanilla-1.21.1/colormap/grass.png` | `assets/minecraft/textures/colormap/grass.png` | `d55308b1c707bc239b4be80ea81dad927827b9010d62f23f34297a99bc1ea4d6` |

The atlas follows the project's block IDs and face order `-X,+X,-Y,+Y,-Z,+Z`.
The grass top and side overlay use the Vanilla Plains tint from the official
colormap. At temperature `0.8` and downfall `0.4`, Minecraft's colormap formula
selects `(50,173)`, RGB `#91BD59`. The grass side base and its separate alpha
overlay remain separate so the renderer can tint and composite the overlay.

Minecraft textures are copyrighted by Mojang Studios/Microsoft. This record
identifies their source and attribution; it does not grant rights beyond those
provided by the rights holders. The files are retained as the requested local
rendering inputs and are not being published separately.

To reproduce the generated C++ header, run
`python3 script/generate_terrain_textures.py --check`. To extract and regenerate
from the pinned official client JAR, run
`python3 script/generate_terrain_textures.py --client-jar /path/to/client.jar`.
