# Core subsystem guide

This is a development map of the current checkout, including local changes; it
does not describe a shipped release. MinecraftClone consumes reusable Core
utilities and Runtime base services from the exact CoreCpp package revision in
[`dependencies.lock.json`](../../dependencies.lock.json). Game-local `core`
contains the remaining ENet transport, GLFW input, and Vulkan layer documented
in [VULKAN.md](VULKAN.md). Public contracts live under
[`src/core/include/core`](../../src/core/include/core) and the installed
CoreCpp package; implementations are in [`src/core`](../../src/core).

## CoreCpp package boundary

`CoreCpp::Core` owns the algorithm, byte/range/value, metadata, macro, file,
formatting, and assertion utilities formerly under this tree. `CoreCpp::Runtime`
owns logging, process-exit registration, and crash handling. The game links both
targets publicly so its shared/server/client headers retain the existing
`<core/...>` and `core::` contracts without source-tree includes. ENet remains
game-local until the Runtime network task, and window/Vulkan code remains
game-local until the corresponding optional Runtime components are ready.

The root CMake configure checks the installed package's clean exact revision
against the lock file. `MC_ALLOW_INEXACT_CORECPP=ON` is reserved for local
development and is not release evidence. The focused
`CoreCpp.ServerOnlyPackageConsumer` test links only `CoreCpp::Runtime` and
rejects optional network, graphics, platform, and audio components.

## Boundaries and build layout

[`src/core/CMakeLists.txt`](../../src/core/CMakeLists.txt) composes the
game-local `net`, `window`, and `vulkan` libraries. The reusable headers and
Runtime sources are supplied by the installed CoreCpp targets. Depend on the
narrowest public header, not a directory-wide umbrella. GLFW/Vulkan platform
bridges remain local until optional Runtime components are integrated.

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
  with acquire/release atomics, and registers `T::destroy()` at app exit; `Net`
  wraps ENet this way.
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

[`Net`](../../src/core/include/core/net/Net.hpp) initializes ENet once; create
network objects after `Net::ensureInit()`. [`Host`](../../src/core/include/core/net/Host.hpp)
owns an `ENetHost`: a null address makes a client host; a bound address accepts
peers. `poll` transfers a received packet into `ReceiveEvent::raw_packet`; the
event's `data` span dies with that event/packet. Consume or copy payloads inside
the callback, never retain `Peer` after ENet disconnects/resets it.

`Host::send` queues work only; call `poll` or `flush` to send. Validate channel
IDs against construction-time `max_channels` and select a valid
[`SendMode`](../../src/core/include/core/net/SendMode.hpp). `Client` owns one
server peer and dispatches only disconnect/receive callbacks; `Server` assigns
monotonic `ClientId`s while connected and dispatches connect/disconnect/receive
callbacks. Both are poll-driven and make no promise of thread safety: serialize
each instance's polling, callbacks, connection state, and sends on its owner
thread. Graceful disconnect/kick waits up to its optional timeout, then resets;
`GenerateEvents` controls callbacks during that wait.

Tests: [`net_tests.cpp`](../../tests/core/net_tests.cpp) and
[`net_client_server_tests.cpp`](../../tests/core/net_client_server_tests.cpp)
exercise loopback connects, reconnects, modes/channels, timeout, broadcast, and
graceful/ungraceful paths.

## Window and input

[`Window`](../../src/core/include/core/window/Window.hpp) owns one GLFW window
and GLFW's global initialization/termination in the current implementation.
Create/destroy windows serially; concurrent/multiple live windows are not an
established contract. `nextFrame()` polls GLFW and returns false when closing.
Resize callbacks run from GLFW event dispatch. With `IgnoreMinimized::Yes`, the
callback blocks in `glfwWaitEvents` until a nonzero framebuffer returns; choose
this only where blocking the event loop is acceptable.

Keyboard/mouse callbacks write process-global atomic state. Query functions may
run concurrently with GLFW callbacks, but input represents the latest sampled
state, not an event queue. `resetMouseDeltas()` is the frame boundary: call it
once per frame after consuming prior deltas to establish the next delta.

Tests: [`keyboard_tests.cpp`](../../tests/core/window/keyboard_tests.cpp) and
[`mouse_tests.cpp`](../../tests/core/window/mouse_tests.cpp).

## Extension checklist

New game-local core code needs explicit lifetime/error contracts, narrow CMake
edges, and a focused suite under `tests/core`; GPU work belongs in [VULKAN.md](VULKAN.md).
