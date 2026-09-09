# Build and verification

Supported application platforms: Windows and macOS. C++23, CMake **3.25+** (preset schema 6), Ninja, vcpkg (`VCPKG_ROOT`), Python 3.12+ for development checks, and Vulkan SDK with loader/headers/`glslc` are required. Text checkouts use LF through `.gitattributes`, keeping hooks and source hashes consistent across platforms. Dependencies are pinned by [vcpkg-configuration.json](../vcpkg-configuration.json); packages are in [vcpkg.json](../vcpkg.json).

```sh
python3 script/ai_setup.py
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure --no-tests=error
python3 script/ai_check.py
```

Run CMake presets from the repository root. Use `release` in place of `debug` for an optimized build. `mc_main` is under `build/<preset>/`; Windows adds `.exe`. Start one `mc_main --server`, then clients with `mc_main`; [gameplay](code/GAMEPLAY.md) lists exact options. The build copies compiled shaders into `shaders/` beside each executable. Runtime lookup uses the executable location, so launch need not use the repository working directory. `cmake --install build/release --prefix <destination>` installs the executable and shaders together; retain that layout when distributing.

After building exact release inputs, use the deterministic [package tooling](PACKAGING.md) to create desktop archives or record Android APK evidence.

## SDK and compiler

Set `VULKAN_SDK` to the SDK platform directory if CMake cannot find it. On macOS, Vulkan runs through MoltenVK; when the SDK is not registered system-wide, set `VK_DRIVER_FILES` to its `share/vulkan/icd.d/MoltenVK_icd.json` for the launched process. Do not commit machine-specific SDK paths.

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

The publisher is [publish.py](../publish.py), not `publish_version.py` as its legacy help text says. Checks-only takes explicit metadata, currently:

```sh
python3 publish.py 'EarlyDev:Initiation' 0.1.0:3 --checks-only
```

`--checks-only` does not build or test; the aggregate gate does both. Development exceptions are limited to dirty Git state and incorrect snapshot files and are reported separately. Do not fabricate a release date or alter source to hide a baseline failure. Other failures block completion.

The existing non-check publisher only accepts `dev` and does not switch to `main` before merging. Do not use it for the AI line. Follow [the explicit snapshot procedure](VERSION_CONVENTION.md); a release-tool repair belongs to its own task.

Additional release gates live in [ai_checks.json](../script/ai_checks.json); disabled screenshot tests are reported as pending. Minor/major gates include environment diagnostics and the recorded docs/environment/hindsight/backlog review. Every commit also uses [task and review receipts](ai/COMMITS.md).

## Test ownership

`mc_tests` uses GoogleTest discovery into CTest. Networking/server tests use local sockets; shader tests load executable-relative assets from another working directory. Input tests drive callbacks without proving interactive controls. Python tooling suites live in `script/tests`.

Enable `MC_ENABLE_RENDERER_SMOKE=ON` at configure time, build, then run `ctest --preset debug -R RendererSmokeTest --output-on-failure`. This optional test requires a desktop, Vulkan device and validation layers; it is not headless. It draws/reloads automatic and forced-vertex pipelines, with CTest rejecting validation/error output. It does not compare pixels or establish resize, user controls, cross-process gameplay or Windows execution from a macOS run.

New test sources must be listed once in [tests/CMakeLists.txt](../tests/CMakeLists.txt); shader/source membership is also checked by the publisher. Keep one test suite/file per source or tightly related unit. No test command is a substitute for checking the required observable result.

Task lookup and preparation use `python3 script/ai_plan.py`; see [project skills](ai/SKILLS.md). Commit hooks include `pre-merge-commit`, so merge commits use the same review/check gates.
