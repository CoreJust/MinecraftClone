# Minecraft Clone 2026

Every programmer should try and develop a Minecraft clone. This one is not my first attempt, but the first serious one.

Written in C++23.

## Configure

```bash
cmake --preset debug
```

## Build

```bash
cmake --build --preset debug
```

## Run

```bash
./build/debug/apps/launcher/mc_launcher
```

## Test

```bash
ctest --preset debug
```

## Dependencies and used tools

CMake is used for building, vcpkg for dependencies management.

Current dependencies:
1. glfw;
2. spdlog;
3. fmt;
4. gtest.

## Plans

Rendering is supposed to be done using Vulkan 1.3.

The project highlights are:
1. **Isolated modules**, allowing to keep implementation inside modules and also giving the ability of hot-reloading in case of a crash;
2. **Replaceable modules**, allowing to create custom implementations;
3. **Modifications** - the game should allow mods, both in terms of configs and full-fledged loadable DLLs;
4. **High performance**.
