# Desktop dependency setup

vcpkg supplies third-party ports. Desktop builds also require the locked
`CoreCpp` and `CoreProject2026` packages in a local install prefix;
[`dependencies.lock.json`](../dependencies.lock.json) records each full commit
and fetch ref. [`script/ci/acquire.py`](../script/ci/acquire.py) verifies source
provenance, builds CoreCpp before CoreProject2026, and installs both packages.
CMake checks their revisions against the lock.

Fetching requires one read-only SSH deploy key from the maintainer for each
private repository. Keep keys outside the checkout and owner-only on POSIX;
the helper uses each key only for its repository.

Presets use `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`. The helper pins
vcpkg to `2b65c20fc66eda893aa15a15a453c3cf09500b19`; bootstrap once in a fresh
local directory:

```sh
mkdir -p .local
export VCPKG_ROOT="$PWD/.local/vcpkg-2b65c20"
python3 script/ci/acquire.py install-vcpkg --root "$VCPKG_ROOT"
python3 script/ai_setup.py
```

The read-only doctor checks Python, CMake, Ninja, and the required toolchain
file. It does not install dependencies or verify the vcpkg revision. For an
existing checkout, confirm `git -C "$VCPKG_ROOT" rev-parse HEAD` prints the
pinned commit above and that the toolchain file exists.

| Desktop platform | `--platform` value | vcpkg triplet |
|---|---|---|
| macOS arm64 | `macos` | `arm64-osx` |
| Windows x64 | `windows` | `x64-windows` |

The following commands show macOS arm64. For Windows set
`MC_DESKTOP_PLATFORM=windows` and `VCPKG_TARGET_TRIPLET=x64-windows`. Use fresh
installed/source roots; commands use POSIX syntax, including Git Bash.

```sh
export MC_DESKTOP_PLATFORM=macos
export VCPKG_TARGET_TRIPLET=arm64-osx
mkdir -p build/debug
export VCPKG_INSTALLED_DIR="$PWD/build/debug/vcpkg_installed"
python3 script/ci/acquire.py install-manifest-dependencies \
    --vcpkg-root "$VCPKG_ROOT" \
    --platform "$MC_DESKTOP_PLATFORM" \
    --installed-root "$VCPKG_INSTALLED_DIR"

export PRIVATE_DEPENDENCY_ROOT="$PWD/build/private-dependencies"
export CORECPP_DEPLOY_KEY="$HOME/.ssh/corecpp-readonly"
export COREPROJECT2026_DEPLOY_KEY="$HOME/.ssh/coreproject2026-readonly"
python3 script/ci/acquire.py fetch-private-dependencies \
    --root "$PRIVATE_DEPENDENCY_ROOT" \
    --corecpp-key "$CORECPP_DEPLOY_KEY" \
    --coreproject2026-key "$COREPROJECT2026_DEPLOY_KEY"
python3 script/ci/acquire.py install-private-dependencies \
    --root "$PRIVATE_DEPENDENCY_ROOT" \
    --platform "$MC_DESKTOP_PLATFORM" \
    --preset debug \
    --cmake-arg="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    --cmake-arg="-DVCPKG_INSTALLED_DIR=$VCPKG_INSTALLED_DIR" \
    --cmake-arg="-DVCPKG_TARGET_TRIPLET=$VCPKG_TARGET_TRIPLET"

export MC_PRIVATE_DEPENDENCIES_PREFIX="$PRIVATE_DEPENDENCY_ROOT/install"
export CORECPP_DIR="$MC_PRIVATE_DEPENDENCIES_PREFIX/lib/cmake/CoreCpp"
export COREPROJECT2026_DIR="$MC_PRIVATE_DEPENDENCIES_PREFIX/lib/cmake/CoreProject2026"
test -f "$CORECPP_DIR/CoreCppConfig.cmake"
test -f "$COREPROJECT2026_DIR/CoreProject2026Config.cmake"
```

CMake discovers the private packages through `CMAKE_PREFIX_PATH`, `CoreCpp_DIR`,
and `CoreProject2026_DIR`; configure with those exact locations:

```sh
cmake --preset debug \
    -DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED_DIR" \
    -DVCPKG_TARGET_TRIPLET="$VCPKG_TARGET_TRIPLET" \
    -DCMAKE_PREFIX_PATH="$MC_PRIVATE_DEPENDENCIES_PREFIX" \
    -DCoreCpp_DIR="$CORECPP_DIR" \
    -DCoreProject2026_DIR="$COREPROJECT2026_DIR"
cmake --build --preset debug
ctest --preset debug --output-on-failure --no-tests=error
python3 script/ai_check.py
```
