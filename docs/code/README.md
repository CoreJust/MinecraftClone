# Code map

Start with one guide; use [the generated file index](INDEX.md) for exact paths. Every source, shader, test, build file, and development script belongs to a module in [modules.json](modules.json).

| Area | Guide | Primary responsibility |
|---|---|---|
| Core utilities, I/O, networking, input | [CORE.md](CORE.md) | Reusable foundations and their contracts |
| Vulkan infrastructure | [VULKAN.md](VULKAN.md) | Resources, builders, pipelines, frame graph, lifetime |
| Entry point, client, server, shared world/protocol | [GAMEPLAY.md](GAMEPLAY.md) | Authority, state transitions, message flow |
| Renderer and shaders | [RENDERING.md](RENDERING.md) | Frame lifecycle, GPU data, reload |
| Build and delivery tools | [../BUILD.md](../BUILD.md) | Prerequisites, commands, checks, limitations |

Guides describe the current checkout, including pre-existing uncommitted work. Consult Git state and version history before treating a behavior as released. Exact signatures remain in headers; no second handwritten API reference is maintained.

The build links a shared `mc` library into `mc_main` and `mc_tests`; logical client/server/core folders are not independently loadable plugins. Mod support and hot reload beyond the existing renderer path remain roadmap ideas. Shared world/protocol code connects server authority and client presentation; reusable core code must not acquire game-specific dependencies.

[Documentation maintenance](../ai/MAINTENANCE.md) defines how to keep this map current. [Vulkan resource recipe](<graphics/How to declare new Vulkan resources.md>) covers wrapper construction.
