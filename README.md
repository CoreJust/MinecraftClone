# Minecraft Clone 2026

A Minecraft-style voxel game written in C++23, with procedural terrain, Vulkan rendering, and a server-authoritative world.

## EarlyDev 0.1.1 BasicBlocks snapshot 2 (released on 12 November 2026)

<!-- Version EarlyDev 0.1.1 BasicBlocks. -->

[Download from GitHub Releases](https://github.com/CoreJust/MinecraftClone/releases)

- **Windows x64.** Run the standalone executable.
- **macOS, Apple silicon.** Extract the archive and open `MinecraftClone.command`.
- **Android arm64.** Install the development-signed APK and connect to a server. [Android setup](docs/ANDROID_ACCEPTANCE.md)

The desktop launchers start a local server. Wait for the start prompt, then press any key.

## Changes in snapshot 2

- Block breaking and placement.
- Creative, Survival, and Spectator modes.
- Adaptive HUD and crosshair.
- Minimap.
- World map with terrain, climate, and tectonic plate layers.
- Cooling lava veins during world generation.
- Terrain relief and river rendering changes.

## Screenshots

**World generation · Relief and cooling lava**

![World generation · Relief and cooling lava](<docs/version_history/EarlyDev 0.1/images/snapshot-2/world-generation-relief.png>)

**World generation · Water**

![World generation · Water](<docs/version_history/EarlyDev 0.1/images/snapshot-2/world-generation-water.png>)

**World generation · Ice**

![World generation · Ice](<docs/version_history/EarlyDev 0.1/images/snapshot-2/world-generation-ice.png>)

**World generation · Arrival**

![World generation · Arrival](<docs/version_history/EarlyDev 0.1/images/snapshot-2/world-generation-arrival.png>)

**Gameplay · Terrain and minimap**

![Gameplay · Terrain and minimap](<docs/version_history/EarlyDev 0.1/images/snapshot-2/gameplay-minimap.png>)

**Gameplay · Block breaking and target outline**

![Gameplay · Block breaking and target outline](<docs/version_history/EarlyDev 0.1/images/snapshot-2/block-breaking.png>)

**Gameplay · Block placement in Creative mode**

![Gameplay · Block placement in Creative mode](<docs/version_history/EarlyDev 0.1/images/snapshot-2/creative-building.png>)

**Gameplay · River terrain from above**

![Gameplay · River terrain from above](<docs/version_history/EarlyDev 0.1/images/snapshot-2/terrain-rivers.png>)

**Gameplay · Creative flight at 500×**

![Gameplay · Creative flight at 500×](<docs/version_history/EarlyDev 0.1/images/snapshot-2/flight-500x.png>)

**World map · Currents layer**

![World map · Currents layer](<docs/version_history/EarlyDev 0.1/images/snapshot-2/world-map-currents.png>)

## Known issues

Terrain can load slowly while traveling, transitions between detail levels remain visible, and the tectonic plate overlay needs work. Movement can be erratic at 500× flight speed.

## Controls

| Action | Control |
| --- | --- |
| Move and look around | **WASD** and the mouse |
| Break or place a block | Hold **left click** to break, **right click** to place |
| Choose a block to place | **B** |
| Switch game modes when permitted | **G** |
| Open or close the world map | **M** |
| Pan and zoom on the map | Drag to pan, scroll to zoom |
| Toggle currents on the world map | **C** |
| Toggle wind on the world map | **Ctrl+W** |
| Copy the current view to the clipboard on Windows or macOS | **F2** |

## Build from source

Desktop builds use C++23, CMake, Ninja, Vulkan, and vcpkg. Install the pinned CoreCpp and CoreLang packages using the desktop dependency guide before configuring CMake. The build guide has the full configure, build, and test commands for desktop and the setup for Android. [Desktop dependencies](docs/DESKTOP_DEPENDENCIES.md) · [Build and test guide](docs/BUILD.md)

<details>
<summary>vcpkg packages</summary>

<!-- Current dependencies: -->

1. enet
2. fmt
3. glfw3
4. glm
5. gmp
6. miniaudio
7. gtest
8. mpfr
9. spdlog
10. spirv-reflect
11. volk
12. vulkan-memory-allocator

</details>

- [Code map](docs/code/README.md) for the client, server, world generation, and renderer.
- [CoreLang scripting](docs/scripting/README.md) for writing world-generation and gameplay scenarios.
- [Roadmap](docs/ROADMAP.md) for what is planned next.
- [Development workflow](docs/ai/README.md) for work on `ai-dev` and snapshots on `ai-main`.

[MIT license](LICENSE)
