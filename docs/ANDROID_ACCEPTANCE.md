# Android runtime acceptance

Use the project AVD `MinecraftClone_S7_ReleaseClean_API_35` for S7 local
acceptance. Resolve the Android SDK once and use its emulator, ADB, and signing
tools together; this also works when `ANDROID_SDK_ROOT` is not already set:

```sh
sdk_root="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
export ANDROID_SDK_ROOT="$sdk_root"
export PATH="$sdk_root/platform-tools:$PATH"
emulator="$sdk_root/emulator/emulator"
adb="$sdk_root/platform-tools/adb"
zipalign="$sdk_root/build-tools/35.0.0/zipalign"
apksigner="$sdk_root/build-tools/35.0.0/apksigner"
export ADB="$adb"
```

The verified macOS profile is a cold boot with no snapshot restore or save,
four virtual cores, 4 GiB RAM, and the host GPU.
Start the emulator in an authorized host execution context. Before starting,
check the chosen serial: if `emulator-5556` is already a `device` and its AVD
name matches the project AVD, reuse it; do not launch another copy. If that
serial belongs to a different AVD, choose and verify a free even console port
instead of stopping or replacing the unrelated emulator:

```sh
"$adb" -s emulator-5556 get-state
"$adb" -s emulator-5556 shell getprop ro.boot.qemu.avd_name
```

Only when no matching AVD is running, start the cold read-only emulator in its
own host terminal:

```sh
"$emulator" -avd MinecraftClone_S7_ReleaseClean_API_35 \
    -read-only -no-snapshot -cores 4 -memory 4096 -gpu host -no-boot-anim \
    -port 5556
```

The restricted
Codex shell may fail the emulator's Crashpad child-port handshake even though
`-list-avds` prints names; that is an emulator-launch context failure, not a
game or APK failure. If another AVD already occupies the default port, select
an unused even console port (the example uses 5556) and verify it is free before
launching. Use `"$adb" devices -l` to discover the serial, verify
`getprop ro.boot.qemu.avd_name` is `MinecraftClone_S7_ReleaseClean_API_35`, and
pass that exact serial to every ADB command.
Before launch, require `sys.boot_completed=1`, `pm path android` to return the
framework APK, `dumpsys -l` to include `package` and `activity`, and every
`adb-agent doctor` subcheck to pass. Android's game client connects to the host
at `10.0.2.2:20040`; launching only the client leaves it waiting in the sky.
Build and launch the optimized host and guarded client together from the
repository root:

```sh
cmake --preset release
cmake --build --preset release
serial=emulator-5556 # replace with the verified serial for the S7 AVD
adb-agent doctor -d "$serial" --json
python3 script/android_pair_playtest.py --serial "$serial"
```

The paired runner uses only `build/release/mc_main` (or `mc_main.exe` on
Windows), refuses a missing Release executable or occupied host UDP port, and
starts one loopback server at render distance 72. It waits for the port to bind
before calling the client guard exactly once, then stays attached to the host
server for the duration of the playtest. Leave the command running while
playing; Ctrl-C stops only the server process that command started. If startup
fails, it reports the reason and cleans up only that same child process. Never
kill an unknown process that already owns port 20040.

The client guard takes a per-user, per-AVD operating-system lock across the
whole check-and-start sequence. Concurrent invocations wait for the lock, then
repeat device, framework, process and activity checks before deciding what to
do; the second invocation therefore reuses the activity started by the first
instead of racing another `am start`. The OS releases the lock if the launcher
exits unexpectedly; an unlocked lock file may remain in the temporary
directory. Within the lock, the guard checks the AVD name, requires all listed
devices to be ready, scans each one for an existing game process, and refuses
cross-device duplicates. It reuses a top-resumed client, brings an existing
background game task forward without creating another activity, or performs
one start when only a leftover process remains without a game activity record.
It waits for the same `NativeActivity` to be top-resumed and in `RESUMED` state.
Do not start another runner, or run a second `app launch` or `am start`, after
the paired command succeeds. If it refuses, resolve the reported device state
rather than retrying launch commands blindly.

Release APKs are unsigned. For emulator-only installation, align and sign a
temporary copy with the stable local Android debug keystore; do not sign or
publish a release artifact with it:

```sh
"$zipalign" -f -p 4 android/app/build/outputs/apk/release/app-release-unsigned.apk \
    /tmp/minecraftclone-s7-aligned.apk
"$apksigner" sign --ks "$HOME/.android/debug.keystore" \
    --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android \
    --out /tmp/minecraftclone-s7-emulator.apk /tmp/minecraftclone-s7-aligned.apk
"$adb" -s <serial> install -r /tmp/minecraftclone-s7-emulator.apk
```

Reuse that same local debug keystore for subsequent installs. A newly generated
one-off key cannot update an existing package and causes
`INSTALL_FAILED_UPDATE_INCOMPATIBLE`. If an older one-off key is already
installed, first verify the exact serial still identifies this read-only
ReleaseClean AVD, then uninstall only `com.corejust.minecraftclone` from that
serial before reinstalling. This resets app data only in the disposable test
instance; never do it on an unrelated emulator or physical device.

S7 Android runtime acceptance covers NativeActivity lifecycle and network
connectivity. It does not establish streamed-terrain visual acceptance:
`AndroidPlayerClient` currently renders the legacy canonical chunk and does
not publish the streamed height-tile meshes. A sky-only post-join frame is
therefore a known Android rendering limitation, not a successful terrain
visual check.

An `am start` success message or a process ID alone is not launch evidence.
The guard’s successful JSON receipt requires `NativeActivity` to be top-resumed
(`state=RESUMED`) and reports its serial and PID. If ActivityManager reports
`failed to attach` or `start timeout`, capture logcat and classify this as an
Android emulator/process-start failure, not a game playtest result. Do not
blindly relaunch while framework services are unresponsive.
