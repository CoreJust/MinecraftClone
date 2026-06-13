# Overview

This is the initiation version where the project structure is outlined and the modules are implemented in a simple way.

## Content additions



## Techincal additions



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

## EarlyDev 0.1.0:3

Basic rendering.
Additions:
1. Some common Vulkan infrastructure in core/vulkan.
2. Window using GLFW.
3. World rendering using Vulkan and GLFW in VulkanRenderer.cpp.
4. Added automatic checks script before publishing snapshots.
