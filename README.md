# Minecraft Clone 2026

A C++23 Minecraft-style game for learning networking, Vulkan graphics, and simulation architecture.

Version EarlyDev 0.1.0 Initiation.

## Development lines

| Branch | Purpose |
|---|---|
| `dev` → `main` | Original development and snapshots |
| `ai-dev` → `ai-main` | AI-led development and verified snapshots |
| `codex/ai-<task>` | Short-lived tasks based on `ai-dev` |

Start with [AI development](docs/ai/README.md), the [backlog](docs/ai/BACKLOG.md), or the [code map](docs/code/README.md). The AI line follows the same [product roadmap](docs/ROADMAP.md); it changes the development workflow, not the intended game.

## Build and run

Requires C++23, CMake 3.25+, Ninja, vcpkg (`VCPKG_ROOT`), and Vulkan SDK including `glslc`. Development tooling requires Python 3.12+. Application builds support Windows and macOS.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure --no-tests=error
python3 script/ai_check.py
```

From the repository root, run `./build/debug/mc_main --server`, then `./build/debug/mc_main` for a client. On Windows use `mc_main.exe`. See [build details](docs/BUILD.md) for SDK setup, configuration options, release-path limitations, and validation.

## Structure

- `src/core`: reusable utilities, networking, window/input, Vulkan infrastructure.
- `src/shared`: world state, protocol, version metadata.
- `src/server`, `src/client`: authoritative simulation and presentation.
- `tests`: game/core tests; `script/tests`: development-tool tests.
- `cmake`, `script`, `.githooks`: build and local validation.
- `docs`: contracts, roadmap, release history, AI planning.

Assets, content packs, modding, replaceable modules, and broader hot reload remain planned directions. Current behavior and limitations are described in the [code guides](docs/code/README.md).

## Dependencies

Current dependencies:
1. enet;
2. fmt;
3. glfw;
4. spdlog;
5. SPIRV-Reflect;
6. glm;
7. gtest;
8. VMA;
9. volk.

The Vulkan SDK supplies the loader and shader compiler; macOS uses MoltenVK. Use a Vulkan 1.3-capable validation environment and test the vertex fallback on devices without mesh shaders. See the build guide for the distinction between requested API version and actual capabilities.

## Project goals

Learn by building a performant, extensible multiplayer game: world generation and updates, network authority under latency, graphics, and progressively richer entities/content. Keep module responsibilities clear as complexity grows. Vulkan 1.4, modding, and replaceable modules are goals rather than statements of current support.

[Code conventions](docs/CODE_CONVENTIONS.md) · [Version/branch convention](docs/VERSION_CONVENTION.md) · [Version history](<docs/version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md>)
