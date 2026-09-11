# Core subsystem guide

This is a development map of the current checkout, including local changes; it
does not describe a shipped release. MinecraftClone consumes reusable Core
utilities and Runtime base services from the exact CoreCpp package revision in
[`dependencies.lock.json`](../../dependencies.lock.json). Game-local `core`
contains only game-specific utility glue. Public contracts live under
[`src/core/include/core`](../../src/core/include/core) and the installed
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
against the lock file. `MC_ALLOW_INEXACT_CORECPP=ON` is reserved for local
development and is not release evidence. The focused
`CoreCpp.ServerOnlyPackageConsumer` test links `CoreCpp::RuntimeNetwork` and
rejects graphics, platform, and audio components.
`MC_BUILD_CLIENT=OFF` is the MinecraftClone headless configuration: it omits
Client sources, Vulkan discovery, and optional CoreCpp graphics/platform
components. `MinecraftClone.ServerOnlyBuild` configures and builds that target
and asserts the forbidden imported and linked targets are absent.

## Boundaries and build layout

[`src/core/CMakeLists.txt`](../../src/core/CMakeLists.txt) contains only the
remaining game-local core target layout. Reusable headers, Runtime sources,
Runtime network transport, and graphics/platform bridges are supplied by
installed CoreCpp targets. Depend on the narrowest public header, not a
directory-wide umbrella.

Core uses assertions for violated programmer contracts. `ASSERT` and
`UNREACHABLE` log a stack trace when logging exists, otherwise write stderr,
then terminate; `HIGH_ASSERT` is compiled out unless enabled. Operational
failure is `bool`/`optional` in I/O and networking, and typed exceptions in
Vulkan.

## Process lifetime, diagnostics, and I/O

- `AtAppExit` is a
  process-global LIFO callback registry. It is intentionally one-shot; callbacks
  registered by `StaticInitializer` and `Log` must be safe during shutdown.
- `StaticInitializer`
  runs `T::init()` exactly once with `std::call_once`, publishes initialization
  with acquire/release atomics, and registers `T::destroy()` at app exit; the
  Runtime network `Net` uses this lifecycle.
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
- `InputSpan`, `SpanUtils`, and `VectorUtils` provide
  zero-copy parameter views and append/byte reinterpretations. `asStringView`
  accepts lvalues and temporary borrowed ranges only; callers retain element
  lifetime and must ensure reinterpretation alignment/size are valid.
- `EnumBits`, `TaggedBool`, `Version`, `TrivialPair`, and `HashCombiner` are
  value types. `TaggedBool` prevents accidental mixing of independent booleans;
  `EnumBits` requires a dense `Count` enum and a fitting unsigned storage type.
- `Defer` executes its captured
  lambda on scope exit. `NonCopyable`/`NonMovable` encode ownership; `Assume`,
  `ConstString`, `Color`, and `CodeExecutionHelper` are lightweight contracts/
  aliases. `ControlFlow` stops polling. `TopoSort`
  returns `nullopt` for cycles; successor indices must be in `[0, node_count)`.

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

New game-local core code needs explicit lifetime/error contracts, narrow CMake
edges, and a focused suite under `tests/core`. GPU policy belongs in
[RENDERING.md](RENDERING.md); never reintroduce a game-local Vulkan/window
ownership layer.
