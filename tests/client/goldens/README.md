# S4 renderer golden references

`s4_scene_v1.ppm` is a P6 Netpbm, 640-by-480, linear RGB reference. Alpha is
implicitly opaque because P6 has no alpha channel. The production renderer
returns RGBA8; the test compares all four channels after restoring the
reference alpha to 255.

TestSupport requires exactly one `# color-space=linear` or `# color-space=srgb`
header declaration and preserves it on read/write. Missing, duplicate, and
unknown declarations fail before comparison; the maximum decoded payload is
64 MiB, with checked arithmetic before allocation or read.

The canonical scene is the current Client S4 draw order: dark-gray grid field,
opaque red player at `(2, 3)`, then opaque green player at `(29, 28)`. At the
approved 640-by-480 extent the grid draw is present but its individual gaps are
not visibly resolved. The debug HUD is explicitly disabled for this image; HUD
behavior remains covered by the dedicated HUD and presentation-smoke tests.

The comparison policy is version 1: linear RGBA8, exact 640-by-480
extent, every channel within 2 encoded values, and zero pixels outside that
tolerance. It is intentionally not a perceptual comparison. A different
format, extent, or color encoding fails rather than silently adapting.

The opt-in target writes bounded diagnostics only when the reference is absent
or mismatched: `s4_scene-actual.ppm`, `s4_scene-diff.ppm` for same-size images
(red means outside the declared tolerance), and `s4_scene-diagnostic.txt`
below the target's build-tree `diagnostics/` directory. It never writes this
checked-in reference.

## Manual generation and approval

Configure a clean, isolated prefix with the target enabled and make unsupported
runtime a failure while generating or approving a reference:

```sh
cmake -S . -B /private/tmp/mc-ai-0052-golden-build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DMC_ENABLE_RENDERER_GOLDEN=ON \
  -DMC_RENDERER_GOLDEN_UNSUPPORTED_POLICY=fail \
  -DVCPKG_MANIFEST_INSTALL=OFF
cmake --build /private/tmp/mc-ai-0052-golden-build --target mc_renderer_golden
ctest --test-dir /private/tmp/mc-ai-0052-golden-build -R '^RendererGoldenTest$' --output-on-failure
```

For a new version, deliberately leave the new reference absent, inspect the
failed test's `renderer-golden/diagnostics/s4_scene-actual.ppm` at native size,
and compare it to the listed scene semantics. A reviewer must record the
renderer source revision, Vulkan driver/device, validation result, command, and
visual approval before manually copying that actual output to a new versioned
reference. Re-run the same strict command afterward. Never copy diagnostics
over an existing reference as a test update, and never use a runtime that
skipped as approval evidence.

`s4_scene_v1.ppm` provenance: generated and manually inspected on macOS arm64
through the foreground MoltenVK presentation/readback path with validation
enabled. Its SHA256 is
`48a162ace9a35d8aaf376da8d9cb48efa45f7f10c0e4ab010e0750d0816b955a`.
The exact command, machine/driver receipt, and semantic approval appear in
MC-AI-0052's completion evidence.

Normal GPU-constrained development may configure
`MC_RENDERER_GOLDEN_UNSUPPORTED_POLICY=skip`; only explicit GLFW setup failure
or the reported unsupported capture states skip. A loaded reference mismatch,
validation output, shader/pipeline error, closed window, timeout, or failed
render remains a failure. This opt-in golden does not replace
`RendererSmokeTest`, which remains the visible presentation smoke.
