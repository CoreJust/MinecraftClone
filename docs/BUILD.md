# Build and verification

Supported application platforms: Windows, macOS, and arm64 Android. Desktop builds require C++23, CMake **3.25+** (preset schema 6), Ninja, vcpkg (`VCPKG_ROOT`), Python 3.12+ for development checks, and a Vulkan SDK with loader/headers/`glslc`. Android builds use the Gradle wrapper, Android SDK API 35, NDK **27.0.12077973**, bundled CMake 3.30.5, Java 21, and vcpkg's pinned builtin `arm64-android` triplet at API floor 28. Text checkouts use LF through `.gitattributes`, keeping hooks and source hashes consistent across platforms. Dependencies are pinned by [vcpkg-configuration.json](../vcpkg-configuration.json); packages are in [vcpkg.json](../vcpkg.json).

```sh
python3 script/ai_setup.py
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure --no-tests=error
python3 script/ai_check.py
```

Run CMake presets from the repository root; use `release` for an optimized
build. Configure with an installed CoreCpp package exposing `CoreCpp::Core` and
`CoreCpp::Runtime`; the configure checks its exact revision against
[`dependencies.lock.json`](../dependencies.lock.json). Local package iteration
may set `-DMC_ALLOW_INEXACT_CORECPP=ON`, which is not release evidence.
`mc_main` is under `build/<preset>/`; `cmake --install` installs it with shaders.

After building exact release inputs, use the deterministic [package tooling](PACKAGING.md) to create desktop archives or record Android APK evidence.
See [runtime acceptance](ACCEPTANCE.md) for scenario, benchmark, and capture commands.

## Android debug APK

Install SDK API 35, NDK 27.0.12077973, and CMake 3.30.5. Set `ANDROID_SDK_ROOT`, Java 21 `JAVA_HOME`, and `VCPKG_ROOT`; do not commit host paths.

```sh
export ANDROID_SDK_ROOT=/path/to/android-sdk
export JAVA_HOME=/path/to/java-21
export VCPKG_ROOT=/path/to/vcpkg
./android/gradlew :app:assembleDebug
```

The result is `android/app/build/outputs/apk/debug/app-debug.apk`: arm64-v8a, API 28 minimum, development-signed only. Gradle derives its version name/code from `PROJECT_VERSION`, including the snapshot index.

Launch options are string extras. `server` is numeric `IP:PORT`; `character` is one of `@#$%&`. Defaults are `10.0.2.2:20040` (emulator host-loopback alias) and `@`:

```sh
adb shell am start -n com.corejust.minecraftclone/android.app.NativeActivity \
    --es server 10.0.2.2:20040 --es character @
```

ENet uses UDP, while `adb reverse` forwards only TCP. Physical devices therefore need a network-reachable server; this snapshot's desktop server binds to loopback, so local acceptance uses the emulator. [Android architecture](code/ANDROID.md) owns NativeActivity lifecycle, input, asset, and renderer constraints.

## SDK and compiler

Set `VULKAN_SDK` to the SDK platform directory if CMake cannot find it. The SDK's `spirv-val` is required by the shader-target CTest checks. On macOS, Vulkan runs through MoltenVK; when the SDK is not registered system-wide, set `VK_DRIVER_FILES` to its `share/vulkan/icd.d/MoltenVK_icd.json` for the launched process. Do not commit machine-specific SDK paths.

## Hosted desktop CI

The GitHub workflow builds and runs CTest for both Debug and Release on `macos-15` arm64 and `windows-2022`. It acquires pinned CMake, Python, vcpkg, hash-verified upstream Ninja archives, and a hash-verified Vulkan SDK, then records the revision and actual tool output in diagnostic artifacts. It validates mesh SPIR-V against Vulkan 1.3 and fallback stages against Vulkan 1.2. Hosted runners set `MC_ENABLE_RENDERER_SMOKE=OFF`: build, CTest, and shader validation are not GPU/runtime acceptance. The workflow uploads only logs and toolchain metadata; packaging and release artifacts have a separate acceptance path.

The renderer requests Vulkan 1.2 and requires dynamic rendering and synchronization2 extensions/features; maintenance4 is not a renderer requirement. Mesh shaders target Vulkan 1.3. Use a Vulkan 1.3-capable validation baseline and exercise the vertex fallback. A lower requested API number does not establish support for every Vulkan 1.2 driver. Vulkan 1.4 remains a project aspiration.

Strict warnings are enabled by presets. `MC_ENABLE_HIGH_ASSERT`, `MC_ENABLE_VULKAN_VALIDATION_LAYERS`, and `MC_ENABLE_SANITIZERS` are CMake options; inspect [cmake helpers](../cmake/Helpers.cmake) and the platform implementation before enabling a configuration. Renderer validation is required in debug builds, with the validation option, or when explicitly requested by renderer options. Ordinary release builds do not require validation layers. Sanitizers and validation are separate evidence from ordinary unit tests.

## Gate levels

| Gate | Evidence |
|---|---|
| Focused CTest `-R <regex>` | The affected existing suite; use while iterating |
| `ai_check.py --fast` | Docs/task consistency, tooling unit tests, whitespace; no application build |
| `ai_check.py` | Fast gate, fresh debug build, full CTest, publisher checks-only |
| `ai_check.py --candidate --level snapshot` | Full precommit snapshot checks; only dirty Git state may fail |
| `ai_check.py --strict --level snapshot` | Same after commit, with a clean tree and valid snapshot metadata |
| Task-specific smoke | Real renderer/network/persistence acceptance, recorded per task |

The publisher is [publish.py](../publish.py). Checks-only takes explicit metadata:

```sh
python3 publish.py 'EarlyDev:Initiation' 0.1.0:3 --checks-only
```

`--checks-only` does not build or test; the aggregate gate does both. Development exceptions are limited to dirty Git state and incorrect snapshot files and are reported separately. Do not fabricate a release date or alter source to hide a baseline failure. Other failures block completion.

The existing non-check publisher only accepts `dev` and does not switch to `main` before merging. Do not use it for the AI line. Follow [the explicit snapshot procedure](VERSION_CONVENTION.md); a release-tool repair belongs to its own task.

Additional [release gates](../script/ai_checks.json) require the windowed smoke below at snapshot, minor, and major levels; S4 task MC-AI-0052 owns offscreen texture goldens. Minor/major gates also record environment, documentation, hindsight, and backlog reviews. Every commit uses [task and review receipts](ai/COMMITS.md).

## Test ownership

`mc_tests` uses GoogleTest discovery into CTest. Networking/server tests use local sockets; shader tests load executable-relative assets from another working directory. `ShaderSpirvTargetTest.*` validates the copied mesh shaders for Vulkan 1.3 and the vertex fallback set for Vulkan 1.2. Input tests drive callbacks without proving interactive controls. Python tooling suites live in `script/tests`.

`script/ai_renderer_smoke.py` rejects foreign caches, configures its preset, builds `mc_renderer_smoke`, and runs `RendererSmokeTest` with bounded timeouts. It records `build/ai-checks/renderer-smoke.log` and fails on missing registration, device, validation, runtime, or timeout errors. This test checks pixels, resize, reload, and close across automatic and vertex-fallback pipelines. It does not prove controls, multiplayer, Windows, or headless goldens; hosted CI remains GPU-free.

New test sources must be listed once in [tests/CMakeLists.txt](../tests/CMakeLists.txt); shader/source membership is also checked by the publisher. Keep one test suite/file per source or tightly related unit. No test command is a substitute for checking the required observable result.

Task lookup and preparation use `python3 script/ai_plan.py`; see [project skills](ai/SKILLS.md). Commit hooks include `pre-merge-commit`, so merge commits use the same review/check gates.
