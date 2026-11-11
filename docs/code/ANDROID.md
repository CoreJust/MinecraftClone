# Android NativeActivity bridge

The [`android/`](../../android) Gradle module packages NativeActivity and loads
`mc_android` through manifest `android.app.lib_name`. Targets are declared in
[`src/CMakeLists.txt`](../../src/CMakeLists.txt) and
[`src/android/CMakeLists.txt`](../../src/android/CMakeLists.txt). They link NDK app
glue and Android libraries; portable game, Vulkan and networking code remains in `mc`.
The `river_diagnostic=first_failure` extra enables [private river diagnostics](RIVER_BEDS.md).

`android_main` creates an [`AndroidPlayerClient`](../../src/android/AndroidPlayerClient.hpp).
The client translates app commands into Vulkan surface lifetime, focus, and
render eligibility. It drains the Android looper before a frame and blocks
on the looper while paused, unfocused, or without a usable window. Teardown
destroys renderer resources before the window is released.

Connection, retry, and join waits also service lifecycle commands and shutdown
requests independently of rendering. A ready foreground surface presents the
existing scene while connecting, without simulating or submitting player input
before an authoritative join. The shared connection attempt retains one ENet
peer for 30 seconds, with lifecycle servicing between transport waits of at
most 25 ms; retry and join waits also service cancellation.

Network messages are serviced only while the render loop is active. Pause,
focus loss and native-window teardown clear movement, held vertical input and
queued actions. Focus loss and window teardown also release keys consumed by
the loading screen. Gameplay resumes on a fresh key press, with automatic
key repeats reserved for map navigation. Window teardown destroys the renderer
and retains the connection and session. The client recreates rendering on resume but does not reconnect if
the server times out during a long background interval. This protocol sends one
normalized movement intent per fixed server tick, so cleared input cannot leave
motion active.

Loading uses the shared native generation animator and `UiPresentation`, including
V10's typed snapshots and stage labels. A matching session/world/L0 supplies the
canonical final S2 map; no legacy elevation-array fallback is used. Its visible
clock pauses without a window or focus and survives window replacement; a new
loading identity resets it. Readiness still requires authoritative state, a fresh
completed prompt frame and a new input edge. `S2MapPresentation` supplies a centered
L0 minimap and world map. Android submits break and place actions through the
shared client. Accepted actions play their material sounds, and tracked break
progress can play chip sounds. Generation audio follows the visible phase.
Touch actions use rendered 48 dp bounds and retain pointer ownership through release/cancel.

`AndroidPlayerClient` creates a CoreCpp `VulkanInstance`, Android surface, and
`PresentationContext` through `RuntimeGraphicsVulkanAndroid`. The Client keeps
the native-window lifetime and asset/input glue; CoreCpp owns the device,
queues, swapchain, bounded frame slots, presentation, and readback. A resize
or window replacement first destroys Client device children and its
`PresentationResourceScope` through the context's pre-recreate hook, then
rebuilds them from a fresh scope in the post-recreate hook.

[`AndroidInput`](../../src/android/AndroidInput.hpp) maps keyboard/gamepad
movement and a left-half touchscreen drag to local movement intent. A
right-half drag supplies yaw/pitch deltas to the client-local 3D camera; the
portable controller lowers left-half intent at the current yaw into the same
normalized authoritative direction packet used by desktop. Server positions
carry fixed-point subcell remainders and are converted to floats only for
camera and rendering presentation.
Back/Escape closes an open map before shutdown; a debounced F1 key-down toggles the
performance HUD and hardware F5 cycles first, rear-third, and front-third
perspectives. R retains its shader-reload action; holding any of these keys
does not repeat its action. The Android client uses the portable vertex path
because `RuntimeKernel::SpirvModule` currently exposes vertex/fragment modules
only.

[`AndroidShaderAssets`](../../src/android/AndroidShaderAssets.hpp) loads `shaders/*.spv` from APK assets and validates SPIR-V alignment and magic.
Gradle stages scene and text shaders, stages the four [runtime audio files](../BUILD.md),
and verifies the audio entries in the debug APK. The ordinary Release variant
passes CMake `Release` and `-O3 -DNDEBUG` for native C and C++ compilation.
Android CMake builds exclude the desktop CTest targets. APK startup evidence
and moving-gameplay performance measurements have separate criteria.

S7's revised release scope requires local Android tests, basic runtime evidence
and the ordinary development APK. Hosted HWASan qualification is suspended.
The retained optional diagnostic HWASan variant generates the NDK `wrap.sh` under the
Android resources source set so the APK contains
`lib/arm64-v8a/wrap.sh`. Only that variant is debuggable and uses legacy JNI
packaging, as required by the [NDK wrap-script contract](https://developer.android.com/ndk/guides/wrap-script);
the HWASan-only Release application ID adds the `.hwasan` suffix so the
diagnostic APK can coexist with the ordinary game installation. The ordinary
Release variant keeps the existing `com.corejust.minecraftclone` application
ID, packaging, and build type.
Diagnostic tooling preflights the packaged wrapper and instrumented AArch64
library before diagnostic startup. It is not the ordinary APK release gate.

The candidate-bound HWASan diagnostic requires authorized macOS ARM64 and
usable Hypervisor.Framework acceleration. It checks Java 21 and provisions a
unique AVD, Android user/configuration homes and pinned SDK under its private
`RUNNER_TEMP`. The emulator requires acceleration on console port 5558. The
job rejects an occupied `emulator-5558` serial or either reserved port and
explicitly targets that serial. An
overall runtime deadline caps every in-loop PID and process-map ADB timeout to
the remaining budget; the verifier fails closed without issuing another device
command after that budget expires. Only successful stdout can provide mapping
evidence; diagnostic-bearing process-map samples are retry gaps and reset the
five-second healthy window. An
always-run cleanup verifies the recorded emulator PID, executable, AVD name,
port, and process start identity before stopping it, then removes only the
run-owned AVD/SDK/user-state subtree. A process already gone at signal time is
treated as stopped; other signal errors fail closed. It leaves unrelated
devices and global AVDs untouched. These requirements follow Android's [emulator acceleration and
port contract](https://developer.android.com/studio/run/emulator-commandline)
and its [AVD/user-home environment variables](https://developer.android.com/tools/variables).
