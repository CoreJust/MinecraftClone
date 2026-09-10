# Snapshot release artifacts

Run the `Snapshot artifacts` workflow manually with the source branch, tag, or
commit and its exact `EPOCH.MAJOR.MINOR:SNAPSHOT` version. A push to `ai-main`,
an `ai/*/*/*` tag, or a purpose-specific `codex/ai-release-*` branch also starts
it without requiring registration on the legacy default branch. The workflow
resolves a manual ref once (or uses the push commit), checks out that
40-character commit in every build job, and rejects a supplied version that
does not match `PROJECT_VERSION`. It does not create a tag or a GitHub Release.

Successful jobs retain separate macOS arm64, Windows x86-64, and Android
arm64-v8a candidate artifacts for 14 days. Desktop candidates contain the
relocatable package, package manifest, SHA-256 sidecar, toolchain record, source
identity, CTest log, and hosted-build evidence. The Android candidate contains
the unchanged CI development-signed APK, APK/package evidence, a deterministic
dependency-license archive, SHA-256 sidecars, toolchain record, and source
identity. Its filename includes `ci-development`: a hosted runner's varying
debug certificate makes it build evidence, not the publishable Android asset.

Desktop builds install the exact CMake 3.31.6 PyPI version and verify its
reported version; that wheel is not selected by a checked-in SHA-256. They
reuse `script/ci/acquire.py` for hash-verified Ninja and Vulkan SDK downloads
and exact vcpkg revision acquisition. Package inputs include the project
license and every installed vcpkg copyright. The macOS package additionally
downloads the Vulkan-Loader and MoltenVK licenses from pinned upstream
revisions and verifies their SHA-256 digests. Windows runtime DLLs are
discovered recursively from the release vcpkg installation and the approved
x64 Visual C++ redistributable directory; unresolved non-system DLLs fail the
package. Evidence records every packaged DLL hash, the redist version, and
Microsoft's official redistribution policy.

Before publishing, download and retain all three candidates, compare every
SHA-256 sidecar, and choose the exact release bytes. Build the publishable
Android APK from the same commit with the existing stable host development
identity; do not export a key or add a CI secret. Generate its schema-1 Android
package evidence and checksum, and disclose that the signature is for
development rather than a trusted store identity. Never publish the CI-only
APK as the final Android asset.

After manual publication, verify the actual GitHub Release before runtime
acceptance. The published, non-draft release must have the exact tag, non-empty
release notes, all three selected platform assets, and each asset's `.sha256`
sidecar. Use the hosted schema-1 release evidence for the two desktop packages
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

Only after this command passes, run the retained exact macOS archive locally
through `MinecraftClone.command`, then install and launch the retained exact
Android APK on the acceptance device or emulator. Their retained hashes must
match the verifier record. The user supplies Windows runtime acceptance; a
hosted build and CTest pass must never be reported as Windows runtime evidence.
Snapshot 8 publication must stop for user feedback and must not finalize or
publish the minor automatically.
