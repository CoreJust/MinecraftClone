# Snapshot release artifacts

Build and qualify releases locally. GitHub hosts source, tags and uploaded
assets; hosted qualification and server-side checks are not release
prerequisites. Windows and macOS are the primary targets. S7 Android requires
local tests, basic install/join/render evidence and the development APK, not
hosted HWASan qualification.

The optional `Snapshot artifacts` workflow is manual build-only tooling, not
the normal delivery path. Do not dispatch it without authorization. Tag and
branch pushes do not start it. When explicitly used, supply a source ref and
its exact `EPOCH.MAJOR.MINOR:SNAPSHOT` version. It resolves that ref once,
checks out the resulting 40-character commit in every build job, and rejects
a version inconsistent with `PROJECT_VERSION`. It creates neither a tag nor
a GitHub Release.

Successful optional jobs retain separate macOS arm64, Windows x86-64, and Android
arm64-v8a candidate artifacts for 14 days. Desktop candidates contain the
relocatable package, package manifest, SHA-256 sidecar, toolchain record, source
identity and hosted build-only evidence. Qualification runs locally; these
jobs do not run CTest. The Android candidate contains
the unchanged CI development-signed APK, APK/package evidence, a deterministic
dependency-and-HUD-license archive, SHA-256 sidecars, toolchain record, and
source identity. The APK itself contains the HUD attribution and Tamsyn font
license under `assets/licenses/hud/`; package evidence verifies both files. Its
filename includes `ci-development`: a hosted runner's varying debug certificate
makes it build evidence, not the publishable Android asset.

The optional hosted desktop builds install CMake 3.31.6 from PyPI and verify its
reported version; that wheel is not selected by a checked-in SHA-256. They
reuse `script/ci/acquire.py` for hash-verified Ninja and Vulkan SDK downloads
and exact vcpkg revision acquisition. Package inputs include the project
license, HUD attribution and Tamsyn font license, and every installed vcpkg
copyright. The macOS package additionally
downloads the Vulkan-Loader and MoltenVK licenses from pinned upstream
revisions and verifies their SHA-256 digests. Windows runtime DLLs are
discovered recursively from the release vcpkg installation and the approved
x64 Visual C++ redistributable directory; unresolved non-system DLLs fail the
package. Evidence records every packaged DLL hash, the redist version, and
Microsoft's official redistribution policy.

Before publishing, build and retain all three selected platform assets using
[the package tools](PACKAGING.md), compare their manifests and SHA-256
sidecars, and complete the required local qualification. Bind final package
evidence to the actual promotion commit. Reuse a build only when its checker
proves the final source inputs and configuration unchanged; never relabel a
different binary or rewrite a failed qualification receipt. Keep original build
and qualification identities alongside final release provenance.

Use the existing stable host development identity for the publishable Android
APK; do not export a key or add a CI secret. Generate its schema-1 package
evidence and checksum, and disclose development signing rather than a trusted
store identity. Never publish a CI-only APK as the final Android asset.

After authorized publication, verify the actual GitHub Release. The published,
non-draft release must have the exact tag, non-empty
release notes, all three selected platform assets, and each asset's `.sha256`
sidecar. Use schema-1 release evidence for the two locally built desktop packages
and the schema-1 package evidence for the final locally signed Android APK:

```sh
python3 script/ci/verify_release_assets.py \
  --repository CoreJust/MinecraftClone \
  --tag ai/EarlyDev/0.1.0/3_26.09.09 \
  --source-commit 0123456789abcdef0123456789abcdef01234567 \
  --evidence artifacts/macos-release-evidence.json \
  --evidence artifacts/windows-release-evidence.json \
  --evidence artifacts/android-package-evidence.json \
  --output artifacts/github-release-verification.json
```

The verifier resolves lightweight and annotated tags to the expected commit,
checks the release and GitHub-reported byte counts, downloads only the six
named asset/checksum files into its own temporary directory, and compares their
exact bytes and SHA-256 values. It neither changes GitHub nor extracts or runs a
downloaded binary. The output record binds the checked repository, release,
tag object, resolved commit, expected evidence hashes, and actual GitHub asset
identities. Any asset replacement invalidates that record and requires another
verification run.

Run the retained exact macOS archive through `MinecraftClone.command` and the
retained exact Android APK on the approved device or emulator before
publication. Their qualified hashes must match the subsequently published
bytes. Publication readback does not require repeating unchanged runtime tests.
The user supplies Windows hands-on acceptance; build and CTest results cannot
substitute for it. Attach the four accepted, labeled S7 screenshots and verify
their uploaded bytes separately from the package verifier.
Snapshot 7 publication must stop for user feedback and must not finalize or
publish the minor automatically.
