# Minecraft Clone 2026

Every programmer should try and develop a Minecraft clone. This one is not my first attempt, but the first serious one.

Written in C++23.

Version EarlyDev 0.1.0 Initiation.

## Project goals

For me, the most important part of this project is the opportunity to learn (and well, have some fun of course).

Why Minecraft? Because it allows to quickly get to the first working version, but then to get it working well and fast and to implement some more complex content you need to carefully think through. Also, Minecraft allows you to focus more on coding rather than creating models/textures or designing game levels.

In general, it easily grows into a large project, which forces you to study to design proper architectures (otherwise you will not be able to make it). I had some other relatively large projects, but they mostly had relatively simple structure. But here the architecture is way more complex - you have to split the responsibilities between the server and the client, organize their interaction in a way that works with network latency and bandwidth limitations. You have to think through the graphics, world update, world generation. You have to manage multiple entities of different kinds and with varying logic (blocks, blocks with complex logic, players, mobs, particles, etc).

Also, I consider this project as a chance to learn some more specific spheres I had no prior experience in:
1. Network (it is my first project that utllizes it);
2. Advanced graphics with Vulkan (previously I only had relatively simple OpenGL experience);
3. Complex multithreading (I had experienced multithreading, but in considerably simpler scenarios).

## Project structure

Project structure (some of the folders may be non-existent for now and will be created later on):

```
assets/ - default game assets, such as textures and sounds.
cmake/ - helper CMake scripts.
config/ - current game settings and configuration files.
content/ - default game content, such as crafting recipes or block properties.
docs/ - used conventions (for code, for versioning, etc), roadmap, version history.
script/ - helper script files.
src/ - actual source files
|---> core/ - foundational files that are relatively project-agnostic (so can be used in other projects easily).
|---> shared/ - shared logic between client and server (e.g. network protocols, world objects).
|---> server/ - implementation of the server-side logic.
|---> client/ - implementation of the client-side logic.
tests/ - unit-tests.
```

Config files are expected to be replaceable in the future, so it can be easy to change the used assets and content. Probably some simple scripts and/or modding will be also added.

## Build

*Note: replace debug with release below to get the Release build.*

### Configure

```bash
cmake --preset debug
```

### Build

```bash
cmake --build --preset debug
```

### Run server

```bash
./build/debug/apps/launcher/mc_main --server
```

### Run

```bash
./build/debug/apps/launcher/mc_main
```

### Test

```bash
ctest --preset debug --output-on-failure
```

## Dependencies and used tools

CMake is used for building, vcpkg for dependencies management.

Current dependencies:
1. enet;
2. fmt;
3. glfw;
4. spdlog;
5. glm;
6. gtest;
7. VMA;
8. Vulkan::glslc;
9. volk.

## Highlights

Rendering is supposed to be done using Vulkan 1.4.

The project highlights are:
1. **Isolated modules**, allowing to keep implementation inside modules and also giving the ability of hot-reloading in case of a crash;
2. **Replaceable modules**, allowing to create custom implementations;
3. **Modifications** - the game should allow mods, both in terms of configs and full-fledged loadable DLLs;
4. **High performance**.
