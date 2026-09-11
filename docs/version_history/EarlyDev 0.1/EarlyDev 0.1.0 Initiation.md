# Overview

This is the initiation version where the project structure is outlined and the modules are implemented in a simple way.

## Release status

Dated entries record snapshot version metadata and contents. For the AI line, the date is assigned during local release preparation; it does not by itself establish promotion, tagging, remote publication, or runtime acceptance. The aggregate task ledger records those results separately under [the version convention](../../VERSION_CONVENTION.md). If promotion moves to another day, update the candidate date before committing and tagging it.

# Snapshots

## EarlyDev 0.1.0:1(26.05.20)

First runnable application. There is no actual server/client yet, no rendering, no world.
Additions:
1. High-level project structure (main folders, MD files, conventions).
2. Proper building with CMake and dependency management with vcpkg.
3. Logging with `spdlog` and `fmt`.
4. Some helper files in `core/` (different macros, assertions, crash handling, etc).

## EarlyDev 0.1.0:2(26.05.25)

Basic network logic and client/server separation.
Additions:
1. Basic network API.
2. Simple 2D console game where real players and bots can roam around a 32x32 world.

## EarlyDev 0.1.0:3(26.09.10)

Basic rendering.
Additions:
1. Vulkan rendering of the existing flat world and players in a GLFW window.
2. Deterministic server and two-client scenarios with renderer smoke, capture, and benchmark evidence.
3. Relocatable macOS and Windows packages plus an arm64-v8a Android Vulkan application.
4. Exact-source snapshot promotion, cross-platform CI, checksums, provenance, and release-asset verification.

## EarlyDev 0.1.0:4(26.09.12)

Controllable 3D presentation of the authoritative flat multiplayer world.
Additions:
1. Deterministic close third-person camera, camera-relative desktop and Android controls, and fixed-step render scheduling.
2. Depth-tested floating platform, grid, and player boxes with a reusable Runtime graphics/kernel path and fixed-size true-offscreen golden coverage.
3. Default-on texture-free shader bitmap debug HUD with windowed FPS, monotonic uptime, player position, and camera angles.
4. CoreLang 0.0.1 scripts, exact reusable CoreCpp/CoreProject2026 packages, and separated Runtime transport/game protocol boundaries.
