# Code map

Start with one guide; use [the generated file index](INDEX.md) for exact paths. Every source, shader, test, build file, and development script belongs to a module in [modules.json](modules.json).

| Area | Guide | Primary responsibility |
|---|---|---|
| Core utilities, I/O, networking | [CORE.md](CORE.md) | Reusable foundations and their contracts |
| Entry point, client, server, shared world/protocol | [GAMEPLAY.md](GAMEPLAY.md) | Authority, state transitions, message flow |
| Native atlas, canonical sampling, layered LOD and maps | [GENERATION.md](GENERATION.md) | Numerical identity, stages, LOD model and reference evidence |
| Renderer, terrain LOD and shaders | [RENDERING.md](RENDERING.md) | Frame lifecycle, atomic scene publication, GPU data, reload |
| Android NativeActivity bridge | [ANDROID.md](ANDROID.md) | Activity lifecycle, input, surfaces, packaged assets |
| Build and delivery tools | [../BUILD.md](../BUILD.md) | Prerequisites, commands, checks, limitations |

Guides describe the current checkout, including pre-existing uncommitted work. Consult Git state and version history before treating a behavior as released. Exact signatures remain in headers and the pinned CoreCpp package; no second handwritten API reference is maintained.

The build separates native generation/sampling, renderer-free gameplay/transport, CPU column-map replay and the graphical application. `mc_main` and `mc_tests` consume the application library; `mc_column_map` links the renderer-free path. [CORE.md](CORE.md) documents those target boundaries. Mod support and hot reload beyond the existing renderer path remain roadmap ideas. Shared world/protocol code connects server authority and client presentation; reusable core code must not acquire game-specific dependencies.

[Documentation maintenance](../ai/MAINTENANCE.md) defines how to keep this map current. Rendering resource lifetime and fence completion receipts are documented in
[RENDERING.md](RENDERING.md). Desktop and Android share the
`TerrainLodRuntime` capture/build/publication path described in
[GAMEPLAY.md](GAMEPLAY.md) and [GENERATION.md](GENERATION.md).

Entity permission declarations and snapshot semantics are documented in [POLICY.md](POLICY.md).
