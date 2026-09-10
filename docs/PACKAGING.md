# Package evidence

`script/package_snapshot.py` turns already-built desktop inputs into a
deterministic archive, or records the identity of an already-built Android
APK. It does **not** configure or build CMake, sign an artifact, upload a
release, install an APK, or establish a runtime smoke result. Build and test
the exact executable first as described in [BUILD.md](BUILD.md).

The command requires an exact source revision and a checked-in or retained
toolchain-evidence JSON object. It does not infer either one from the current
checkout or a locally installed SDK. Outputs are local files only.

## Desktop archives

Pass the built executable (normally from an explicit CMake install prefix),
the shader directory installed beside it, every non-system runtime dependency,
and all applicable license material. Input files and trees must be regular,
non-empty where required, and free of symlinks. The result contains
`manifest.json` with the platform, version, source revision, normalized
toolchain evidence, runtime layout, licenses, and SHA-256/size of every shipped
file. A small adjacent `<archive>.manifest.json` reports the archive SHA-256.

Windows packages use ZIP and place the executable and supplied dependency DLLs
beside one another for normal DLL lookup:

```sh
python3 script/package_snapshot.py desktop \
  --platform windows --format zip \
  --executable build/release/mc_main.exe \
  --shaders build/release/shaders \
  --runtime path/to/required.dll \
  --license LICENSE --license path/to/dependency/LICENSE \
  --version 0.1.0:3 --source-commit <40-lowercase-hex-revision> \
  --toolchain-evidence build/release/toolchain-evidence.json \
  --output dist/minecraftclone-0.1.0-3-windows.zip
```

The archive includes `LAUNCH.txt`: a compatible vendor GPU driver remains an
external requirement and is never bundled. A successful archive only proves
its contents and hashes; it is not Windows runtime evidence. Names that collide
under Windows case folding, reserved device names, and other Windows-invalid
archive components are rejected before writing.

macOS packages use a `tar.gz` archive and need explicit Vulkan loader,
MoltenVK, and ICD inputs in addition to any other dylib dependencies:

```sh
python3 script/package_snapshot.py desktop \
  --platform macos --format tar.gz \
  --executable build/release/mc_main \
  --shaders build/release/shaders \
  --vulkan-loader /Users/core-just/VulkanSDK/1.4.357.0/macOS/lib/libvulkan.1.4.357.dylib \
  --moltenvk /Users/core-just/VulkanSDK/1.4.357.0/macOS/lib/libMoltenVK.dylib \
  --icd-json /Users/core-just/VulkanSDK/1.4.357.0/macOS/share/vulkan/icd.d/MoltenVK_icd.json \
  --runtime path/to/other-runtime.dylib \
  --license LICENSE --license path/to/MoltenVK-LICENSE \
  --version 0.1.0:3 --source-commit <40-lowercase-hex-revision> \
  --toolchain-evidence build/release/toolchain-evidence.json \
  --output dist/minecraftclone-0.1.0-3-macos.tar.gz
```

The macOS command runs `lipo -archs` and `otool -L` over the executable and
bundled dylibs. Every bundled dylib must contain every supported architecture
used by the executable (`arm64` or `x86_64`); universal dylibs are accepted for
a thin executable, but missing an executable architecture is rejected. Binaries
may depend only on system libraries or `@rpath`/loader-relative paths. It
rejects an embedded developer path, and every relative dylib dependency must
name an explicitly bundled library. The bundled regular loader bytes are always
named `lib/libvulkan.1.dylib`, the stable fallback name used by Volk; source
symlinks are not accepted. The bundled ICD is rewritten to reference
`../../lib/libMoltenVK.dylib`, never the input SDK path.

`MinecraftClone.command` locates its own directory, sets the bundled Vulkan
loader and ICD paths, and launches `mc_main`; it does not depend on the current
working directory or `VULKAN_SDK`. Extract and launch that exact archive before
recording macOS runtime acceptance.

Archives are sorted and use fixed timestamps, ownership, and modes. Supplying
identical input bytes and identical provenance produces identical bytes. Input
files are snapshotted once, and the manifest and archive use those same bytes;
macOS binary inspection runs over the snapshots rather than mutable source paths. The
tool rejects an existing output unless `--overwrite` makes replacement
explicit. Archive and evidence writes use exclusive randomized temporary files
and atomic replacement; output and sidecar symlinks are rejected.
Every output-parent ancestor must also be a real directory rather than a symlink;
use its canonical path when a system alias such as `/tmp` is involved.

## Android APK evidence

Android input is an existing APK, not a packaging source. This command hashes
the APK and writes an evidence JSON file without copying, transforming,
resigning, installing, or uploading it:

```sh
python3 script/package_snapshot.py android \
  --apk android/app/build/outputs/apk/debug/app-debug.apk \
  --api-level 35 --abi arm64-v8a \
  --signing development --source-exactness exact \
  --version 0.1.0:3 --source-commit <40-lowercase-hex-revision> \
  --toolchain-evidence android/build/toolchain-evidence.json \
  --output dist/minecraftclone-0.1.0-3-android-evidence.json
```

`--signing` records only `development` or `unknown`; it never invents a signing
identity. `--source-exactness` records whether the caller has established that
the APK came exactly from `--source-commit`. Before writing evidence, the tool
reads only the bounded ELF headers of packaged native libraries and requires
their architectures to match both the `lib/<abi>/` directories and the exact
set supplied through `--abi`; every declared ABI must contain
`libmc_android.so`. The output records that validated ABI set with the original
APK byte count and SHA-256. Device install and launch evidence belongs to the
Android task, not this command.

## Focused tooling checks

```sh
python3 -m unittest script.tests.test_package_snapshot -v
```

The fixtures cover deterministic Windows ZIP contents, macOS relative ICD and
self-locating launcher construction, unsafe-input rejection, Android byte
preservation, and APK native-library ABI validation. They mock macOS binary
inspection; run an actual archive command on macOS for `otool`/`lipo` evidence.
