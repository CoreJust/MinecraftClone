# Core subsystem guide

This is a development map of the current checkout, including local changes; it
does not describe a shipped release. MinecraftClone consumes reusable Core
utilities and Runtime base services from the exact CoreCpp package revision in
[`dependencies.lock.json`](../../dependencies.lock.json). The obsolete
game-local `src/core` tree has been removed. Public contracts live in the installed
CoreCpp package; graphics, platform windows, Vulkan presentation, and shader
loading are supplied by installed optional Runtime components.

## CoreCpp package boundary

`CoreCpp::Core` owns the algorithm, byte/range/value, metadata, macro, file,
formatting, and assertion utilities formerly under this tree. `CoreCpp::Runtime`
owns logging, process-exit registration, and crash handling. The game links both
targets publicly so its shared/server/client headers retain the existing
`<core/...>` and `core::` contracts without source-tree includes.
`CoreCpp::RuntimeNetwork` owns the generic transport/session service.
Desktop Client links `RuntimeGraphics`, `RuntimeGraphicsVulkan`,
`RuntimeGraphicsVulkanGlfw`, `RuntimePlatformGlfw`, and `RuntimeKernel`.
Android Client substitutes `RuntimeGraphicsVulkanAndroid`; server-only
consumers remain free of every graphics, platform, Vulkan, GLFW, and audio
target.

The root CMake configure checks the installed package's clean exact revision
against the lock file, including local development builds.
`MC_ALLOW_INEXACT_CORECPP` remains a legacy cache flag but cannot bypass those
checks. The focused
`CoreCpp.ServerOnlyPackageConsumer` test links `CoreCpp::RuntimeNetwork` and
rejects graphics, platform, and audio components.
`MC_BUILD_CLIENT=OFF` omits Vulkan discovery and optional CoreCpp graphics/platform
components. `MC_BUILD_COLUMN_MAP` defaults to the selected client-build setting;
set it explicitly to `ON` with graphics disabled to build the CPU-only
`mc_column_map` normal-game consumer. This consumer uses audio output for the
shared join cue, while the server-only configuration disables both options and
retains the audio exclusion. `MinecraftClone.ServerOnlyBuild` configures and builds that target
and asserts the forbidden imported and linked targets are absent.

## Boundaries and build layout

`mc_generation` and `mc_world_sampling` own the native numerical kernels and
canonical sampler. `mc_terrain_corelang` owns bounded backend execution and the
generation-plan adapter; it also supports a standalone CPU build without game
or graphics startup. `mc_game_core` consumes that target and owns shared
simulation, protocol, server services and the renderer-free client lifecycle. `mc_column_replay` adds pure
replay inputs and the normal-game CPU column-map adapter. The `mc` application
library adds platform/presentation adapters; graphics dependencies belong there.
`mc_column_map` configures and links with graphics disabled and uses the same
server protocol and materialized world as the graphical client.

The client-side `TerrainLodSource` asynchronously builds a separate atlas from
the exact session configuration and publishes only matching identity/epoch
results. It duplicates server atlas work; it does not replace server-owned
chunks, collision, edits, or state. Keep this preview adapter separate from
server `WorldPreparation` and renderer ownership.


Reusable headers, Runtime sources,
Runtime network transport, and graphics/platform bridges are supplied by
installed CoreCpp targets. Depend on the narrowest public header, not a
directory-wide umbrella.

Core uses assertions for violated programmer contracts. `ASSERT` and
`UNREACHABLE` log a stack trace when logging exists, otherwise write stderr,
then terminate; `HIGH_ASSERT` is compiled out unless enabled. Operational
failure is `bool`/`optional` in I/O and networking, and typed exceptions in
Vulkan.

## Process lifetime, diagnostics, and I/O

- `AtAppExit` runs process-global LIFO callbacks once. `StaticInitializer`
  initializes via `std::call_once`, publishes with acquire/release atomics and
  registers teardown; callbacks must remain safe during shutdown.
- `Log` owns the global asynchronous
  spdlog logger. `ensureInit` is idempotent, optionally creates `game.log`, and
  arranges teardown; logging macros silently do nothing before initialization.
  Its sink/logger mutation API is not an application-wide synchronization
  protocol—configure it during startup. `CrashHandler`
  installs desktop signal/Windows handlers that terminate immediately with a nonzero status.
  The POSIX signal path writes only one fixed stderr message before `_Exit`; neither path logs,
  captures a stacktrace, allocates, takes locks, or runs exit callbacks. Signal-time diagnostics are
  therefore deliberately limited, while normal `main` cleanup remains separate.
- `File` reads binary files. `readFile`
  allocates the exact reported size; `readFileTo` requires a destination large
  enough for the entire file and returns `false` for missing/open/read/close
  failures. Both log the failure. Formatting adapters next to `Log.hpp` extend
  `fmt` for core values.

Tests: [`crash_handler_tests.cpp`](../../tests/core/crash_handler_tests.cpp),
[`io_tests.cpp`](../../tests/core/io_tests.cpp), [`log_tests.cpp`](../../tests/core/log_tests.cpp), and
[`static_initializer_tests.cpp`](../../tests/core/static_initializer_tests.cpp).

## Value, byte, range, and control utilities

- `ByteWriter` writes fixed-width scalar `ByteSerializable` values. Spans/strings are a
  `uint64_t` element count followed by raw bytes. `ByteReader`
  advances only after a complete item and rejects truncated or oversized
  prefixes. Returned span/string views borrow the reader's input bytes; preserve
  that input for their whole use. This is native in-memory serialization, not a
  portable wire format: endianness remains part of its contract; enums and
  aggregates require dedicated codecs.
- `InputSpan`, `SpanUtils`, and `VectorUtils` expose borrowed ranges; callers
  preserve storage lifetime, alignment and size. `asStringView` rejects temporary
  owning ranges.
- `EnumBits` requires a dense `Count` enum and fitting storage; `TaggedBool`
  distinguishes boolean meanings. Other small value types retain narrow headers.
- `Defer` runs at scope exit. Ownership markers prohibit copying/moving.
  `TopoSort` rejects cycles and requires valid successor indices.

Tests: [`byte_io_tests.cpp`](../../tests/core/byte_io_tests.cpp),
[`range_span_tests.cpp`](../../tests/core/range_span_tests.cpp),
[`algorithm_tests.cpp`](../../tests/core/algorithm_tests.cpp), and the focused
`*_tests.cpp` files in [`tests/core`](../../tests/core).

## Networking

`CoreCpp::RuntimeNetwork` owns `Net`, `Host`, `Client`, and `Server`; create
network objects after `Net::ensureInit()`. A null `Host` address creates a
client host; a bound address accepts peers. Receive events own their
`std::vector<uint8_t>` payload, so Shared decodes bytes without depending on a
native packet layout. Never retain `Peer` after disconnect or reset.

`Host::send` queues work only; call `poll` or `flush` to send. Channel limits,
send modes, poll-driven lifecycle, graceful disconnect/kick bounds, and
single-owner-thread serialization are RuntimeNetwork contracts. MinecraftClone
Shared owns game-message encoding, validation, and authoritative player mapping.

Tests: [`net_tests.cpp`](../../tests/core/net_tests.cpp) and
[`net_client_server_tests.cpp`](../../tests/core/net_client_server_tests.cpp)
exercise installed RuntimeNetwork loopback, reconnect, modes/channels, timeout,
broadcast, and graceful/ungraceful paths.

## Extension checklist

New core contracts need focused tests and narrow dependencies. GPU ownership
belongs in [RENDERING.md](RENDERING.md) and installed Runtime components.
