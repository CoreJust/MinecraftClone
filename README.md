# Minecraft Clone 2026

A C++23 Minecraft-style game for learning networking, Vulkan graphics, and simulation architecture.

Version EarlyDev 0.1.0 Initiation.

## Gameplay

![Snapshot 4 close third-person gameplay with the debug HUD](<docs/version_history/EarlyDev 0.1/images/earlydev-0.1.0-4-third-person-hud.png>)

![Snapshot 4 oblique view of the floating platform and local player](<docs/version_history/EarlyDev 0.1/images/earlydev-0.1.0-4-oblique-platform.png>)

These are actual Snapshot 4 gameplay captures from its visual-acceptance work. The [version history](<docs/version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md>) records their context and the exact release evidence separately.

## Development lines

| Branch | Purpose |
|---|---|
| `dev` → `main` | Original development and snapshots |
| `ai-dev` → `ai-main` | AI-led development and verified snapshots |
| `codex/ai-<task>` | Short-lived tasks based on `ai-dev` |

Start with [AI development](docs/ai/README.md), the [backlog](docs/ai/BACKLOG.md), the [code map](docs/code/README.md), or the [scenario scripting guide](docs/scripting/README.md). The AI line follows the same [product roadmap](docs/ROADMAP.md); it changes the development workflow, not the intended game.

## Build and run

Requires C++23, CMake 3.25+, Ninja, vcpkg (`VCPKG_ROOT`), and Vulkan SDK including `glslc`. Development tooling requires Python 3.12+. Application builds support Windows, macOS, and arm64 Android.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure --no-tests=error
python3 script/ai_check.py
```

From the repository root, run `./build/debug/mc_main --server` (optional `--port PORT`), then `./build/debug/mc_main` for the graphical player client. Use `--player-client` for the explicit graphical mode or `--bot-client` for a headless bot; either client accepts `--address IP:PORT` and defaults to `127.0.0.1:20040`. On Windows use `mc_main.exe`. See [build details](docs/BUILD.md) for SDK setup, configuration options, release-path limitations, and validation.

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
9. volk;
10. GMP;
11. MPFR.

The Vulkan SDK supplies the loader and shader compiler; macOS uses MoltenVK. Use a Vulkan 1.3-capable validation environment and test the vertex fallback on devices without mesh shaders. See the build guide for the distinction between requested API version and actual capabilities.

## Project goals

Learn by building a performant, extensible multiplayer game: world generation and updates, network authority under latency, graphics, and progressively richer entities/content. Keep module responsibilities clear as complexity grows. Vulkan 1.4, modding, and replaceable modules are goals rather than statements of current support.

[Code conventions](docs/CODE_CONVENTIONS.md) · [Version/branch convention](docs/VERSION_CONVENTION.md) · [Version history](<docs/version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md>)


![S5 stone world gameplay](docs/version_history/EarlyDev%200.1/assets/s5-stone-world.png)

S5 visible runtime overview: a seed-42 air-and-stone flight scene rendered with the original mottled 16-pixel stone texture. The frame comes from the controlled runtime camera during local acceptance.
