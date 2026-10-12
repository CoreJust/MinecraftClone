# Build and verification

Supported application platforms: Windows, macOS, and arm64 Android. Desktop builds require C++23, CMake **3.25+** (preset schema 6), Ninja, vcpkg (`VCPKG_ROOT`), Python 3.12+ for development checks, and a Vulkan SDK with loader/headers/`glslc`. Android uses the Gradle wrapper, Android SDK API 35, NDK **27.0.12077973**, bundled CMake 3.30.5, Java 21, and vcpkg's builtin `arm64-android` triplet (API 28 floor). `.gitattributes` enforces LF. [vcpkg configuration](../vcpkg-configuration.json) pins dependencies and the Windows GMP overlay; [manifest](../vcpkg.json) lists packages.

```sh
python3 script/ai_setup.py
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure --no-tests=error
python3 script/ai_check.py
```

Run CMake presets from the repository root; use `release` for an optimized
build. Configure packages exposing `CoreCpp::Core`, `CoreCpp::Runtime`, and
`CoreProject2026::CoreLang`; CMake checks their exact SHAs against
[`dependencies.lock.json`](../dependencies.lock.json). Schema 2 uses `fetch_ref`
locators for validated branches; CI fetches their history but builds only locked commits.
CoreProject2026 rejects dirty, unknown, or mismatched packages.
`-DMC_ALLOW_INEXACT_CORECPP=ON` is development-only.
Standard presets keep vcpkg installs under `build/<preset>/vcpkg_installed`.
`mc_main` is under `build/<preset>/`; `cmake --install` installs it with shaders.

After building exact release inputs, use the deterministic [package tooling](PACKAGING.md) to create desktop archives or record Android APK evidence.
See [runtime acceptance](ACCEPTANCE.md) for scenario, benchmark, and capture commands.

## Android debug APK

Set Android SDK/NDK, Java 21, vcpkg, and pinned Android CoreCpp/CoreProject2026
paths as below; vcpkg's compiler probe requires `ANDROID_NDK_HOME`.

```sh
export ANDROID_SDK_ROOT=/path/to/android-sdk
export JAVA_HOME=/path/to/java-21
export VCPKG_ROOT=/path/to/vcpkg
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/27.0.12077973"
export MC_CMAKE_PREFIX_PATH=/path/to/android-dependency-prefix
export CORECPP_DIR="$MC_CMAKE_PREFIX_PATH/lib/cmake/CoreCpp"
export COREPROJECT2026_DIR="$MC_CMAKE_PREFIX_PATH/lib/cmake/CoreProject2026"
test -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
test -f "$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
test -f "$CORECPP_DIR/CoreCppConfig.cmake"
test -f "$COREPROJECT2026_DIR/CoreProject2026Config.cmake"
./android/gradlew \
    -PcmakePrefixPath="$MC_CMAKE_PREFIX_PATH" \
    -PcoreCppDir="$CORECPP_DIR" \
    -PcoreProjectDir="$COREPROJECT2026_DIR" \
    :app:assembleDebug
```

The APK is arm64-v8a, API 28 minimum, and development-signed; its version derives from `PROJECT_VERSION`.

`assembleRelease` selects native `Release` (`-O3 -DNDEBUG`) and stages HUD
licenses before lint; signing is unchanged.

Launch options are string extras. `server` is numeric `IP:PORT`; `character` is one of `@#$%&`. Defaults are `10.0.2.2:20040` (emulator host-loopback alias) and `@`:

```sh
adb shell am start -n com.corejust.minecraftclone/android.app.NativeActivity \
    --es server 10.0.2.2:20040 --es character @
```

See the [Android runbook](ANDROID_ACCEPTANCE.md).
See the [macOS graphical acceptance runbook](MACOS_ACCEPTANCE.md).

ENet uses UDP, while `adb reverse` forwards only TCP. Physical devices need a network-reachable server; this snapshot's desktop server binds to loopback, so local acceptance uses the emulator. [Android architecture](code/ANDROID.md) owns NativeActivity lifecycle, input, asset, and renderer constraints.

## SDK and compiler

Set `VULKAN_SDK` to the SDK platform directory if CMake cannot find it. The SDK's `spirv-val` is required by the shader-target CTest checks. On macOS, Vulkan runs through MoltenVK; when the SDK is not registered system-wide, set `VK_DRIVER_FILES` to its `share/vulkan/icd.d/MoltenVK_icd.json` for the launched process. Do not commit machine-specific SDK paths.

## Artifact-only hosted builds

Artifact CI uses `macos-15` arm64/`windows-2022`, pinned tools and cached dependencies. Jobs build/package artifacts and verify provenance/checksums/exclusions; qualification tests, analyzers and runtime acceptance run locally. macOS needs GMP/MPFR autotools; Windows needs the GMP overlay, VS 2022 x64 and Vulkan runtime ZIP/license. CoreLang excludes legacy Script. Android initializer warnings remain nonfatal. Linux is analysis-only.

ASan requires instrumented Release vcpkg/private dependencies with matching
runtime/iterator/annotation settings, `/Zi` compiler and `/DEBUG` linker symbols.
C++ `/analyze /W4 /WX /EHsc` uses Debug dependencies; `/analyze:external-`
excludes imported/system headers only. Nested builds retain flags and run serially;
qualify both Windows rows before promotion.

The renderer requests Vulkan 1.2 with dynamic rendering and synchronization2 extensions/features, not maintenance4. Mesh shaders target Vulkan 1.3; validate on Vulkan 1.3 and exercise vertex fallback. Requesting Vulkan 1.2 does not establish support for every such driver. Vulkan 1.4 remains aspirational.

[Helpers](../cmake/Helpers.cmake) enforce strict warnings and sanitizer/validation options. Debug/explicit builds require validation; Release does not. Local Mac ASan retains LSan. Linux LSan selects `MC_LSAN_LIBCXX_PREFIX`. MSan uses pinned instrumented libc++/libc++abi with system `libgcc_s`, not patched libunwind. Runtime/stock-ignorelist bytes are captured; no suppression is added. Process-local `DEBUGINFOD_URLS=` keeps symbolization offline. Exception/rethrow/cleanup and symbolized heap-read/origin controls precede game tests. Receipts bind selected Vulkan/shader-tool bytes and Windows ASan DLLs. See [setup findings](ai/S7-DELIVERY-PROCESS-REPAIR.md).

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

`--checks-only` skips builds/tests; aggregate gates run both. [AI release policy](ai/RELEASES.md) binds dates to validated source commits in Europe/Belgrade; development and legacy checks use today. Development exceptions: dirty Git state and incorrect snapshot files, reported separately. Other failures block completion. Never fabricate dates or hide baseline failures.

Do not use the legacy non-check publisher for the AI line; follow [snapshot publication](VERSION_CONVENTION.md).

Additional [release gates](../script/ai_checks.json) require the windowed smoke below at snapshot, minor, and major levels; S4 task MC-AI-0052 owns offscreen texture goldens. Minor/major gates also record environment, documentation, hindsight, and backlog reviews. Every commit uses [task and review receipts](ai/COMMITS.md).

## Test ownership

`mc_tests` uses GoogleTest discovery into CTest. Networking tests use local sockets; shader tests load executable-relative assets from another directory. `ShaderSpirvTargetTest.*` validates mesh shaders for Vulkan 1.3 and vertex fallbacks for Vulkan 1.2. Input callbacks do not prove interactive controls. Python suites live in `script/tests`.

Timed S7 tests use CTest `RUN_SERIAL`; workloads remain unchanged.

Local diagnostics are not release qualification; retained
[observation patches](ai/tasks/MC-AI-0433.md) must not enter production builds.

`script/ai_renderer_smoke.py` rejects foreign caches and builds/runs `RendererSmokeTest` with bounded timeouts, logging to `build/ai-checks/renderer-smoke.log`. Missing registration/device, validation, runtime, and timeout errors fail. Pixels, resize, reload, and close cover automatic/vertex-fallback pipelines—not controls, multiplayer, Windows, or headless goldens. Hosted CI remains GPU-free.

`MC_ENABLE_RENDERER_GOLDEN=ON` enables S4 readback against manual
non-updating reference. `MC_RENDERER_GOLDEN_UNSUPPORTED_POLICY=fail` approves;
`skip` is only unsupported. Mismatches fail.
[golden guide](../tests/client/goldens/README.md) owns policy.

New test sources must be listed once in [tests/CMakeLists.txt](../tests/CMakeLists.txt); shader/source membership is also checked by the publisher. Keep one test suite/file per source or tightly related unit. No test command is a substitute for checking the required observable result.

Task lookup and preparation use `python3 script/ai_plan.py`; see [project skills](ai/SKILLS.md). Commit hooks include `pre-merge-commit`, so merge commits use the same review/check gates.
