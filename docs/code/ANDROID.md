# Android NativeActivity bridge

The Android application is a NativeActivity packaged by the Gradle module in
[`android/`](../../android). Its shared object is `mc_android`; the manifest
loads it through the `android.app.lib_name` metadata entry. The source target
is declared in [`src/CMakeLists.txt`](../../src/CMakeLists.txt) and augmented
by [`src/android/CMakeLists.txt`](../../src/android/CMakeLists.txt). It links
the NDK native app glue plus the Android system libraries, while the portable
game, Vulkan, networking, and renderer code remain in `mc`.

`android_main` creates an [`AndroidPlayerClient`](../../src/android/AndroidPlayerClient.hpp).
The client translates app commands into Vulkan surface lifetime, focus, and
render eligibility. It drains the Android looper before a frame and blocks
on the looper while paused, unfocused, or without a usable window. Teardown
destroys renderer resources before the window is released.

Network messages are serviced only while the render loop is active. Pause or
focus loss clears input before blocking; losing the native window destroys the
renderer. The client recreates rendering on resume but does not reconnect if
the server times out during a long background interval. This protocol sends one
normalized movement intent per fixed server tick, so cleared input cannot leave
motion active.

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
Back/Escape requests client shutdown; a debounced F1 key-down toggles the
performance HUD, enabled by default for normal gameplay, while R retains its
shader-reload action (holding either key does not repeat its action). The Android client uses the portable vertex path
because `RuntimeKernel::SpirvModule` currently exposes vertex/fragment modules
only.

[`AndroidShaderAssets`](../../src/android/AndroidShaderAssets.hpp) reads
bare `.spv` names from the APK asset manager under `shaders/`, validates their
SPIR-V word alignment and magic, and returns the portable renderer input. The
Gradle source set stages all five selected vertex/fragment shader sources,
including `debug_hud.vert` and `debug_hud.frag`, so the APK contains their
compiled `shaders/*.spv` assets; see the Android section of [the build
guide](../BUILD.md) for packaging and environment requirements.
