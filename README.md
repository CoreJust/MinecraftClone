# Minecraft Clone 2026

A C++23 Minecraft-style game for learning networking, Vulkan graphics, and simulation architecture.

Version EarlyDev 0.1.1 BasicBlocks.

## Play Snapshot 1

On Windows, double-click the standalone **MinecraftClone S1** executable. It starts a local server and opens the game automatically. No separate server or console is needed. Wait for the centered start prompt, then press a key or mouse button.

S1 introduces generated continents, climate and surface materials, a world-generation map, textured terrain with distant detail levels, and CoreLang 0.1.5 integration. Full-detail terrain targets a 40-chunk (640-block) range around the player.

![Snapshot 1 grassy block terrain near the canonical spawn](<docs/version_history/EarlyDev 0.1/images/earlydev-0.1.1-1-terrain.png>)

This current S1 production-renderer capture uses a 24-chunk diagnostic view. [Capture context and snapshot history](<docs/version_history/EarlyDev 0.1/EarlyDev 0.1.1 BasicBlocks.md>) distinguish it from ordinary playtest evidence. Earlier snapshots remain in their own history pages.

The owner accepted S1 for release with known performance, streaming and LOD defects. Frame pacing, continuous full-detail coverage, LOD seams and material transitions, focus recovery, and outstanding platform qualification are explicit [S2 targets](docs/ai/decisions/earlydev_0_1_1_s2_delivery_targets.md). The [S2 plan](docs/ai/decisions/earlydev_0_1_1_s2.md) also records the parallel design work.

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

From the repository root, run `./build/debug/mc_main` for local singleplayer. Use `--server` to host a dedicated server (optional `--port PORT`), or `--player-client --address IP:PORT` to join one. Use `--bot-client` for a headless client. On Windows use `mc_main.exe`. See [build details](docs/BUILD.md) for SDK setup, configuration options, release-path limitations, and validation.

## Structure

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
11. MPFR;
12. miniaudio.

The Vulkan SDK supplies the loader and shader compiler; macOS uses MoltenVK. Use a Vulkan 1.3-capable validation environment and test the vertex fallback on devices without mesh shaders. See the build guide for the distinction between requested API version and actual capabilities.

## Project goals

Learn by building a performant, extensible multiplayer game: world generation and updates, network authority under latency, graphics, and progressively richer entities/content. Keep module responsibilities clear as complexity grows. Vulkan 1.4, modding, and replaceable modules are goals rather than statements of current support.

[Code conventions](docs/CODE_CONVENTIONS.md) · [Version/branch convention](docs/VERSION_CONVENTION.md) · [Version history](<docs/version_history/EarlyDev 0.1/EarlyDev 0.1.0 Initiation.md>)
