# Core subsystem guide

This is a development map of the current checkout, including local changes; it
does not describe a shipped release. `core` supplies platform glue, small
value/compile-time utilities, logging and file I/O, ENet transport, GLFW input,
and the Vulkan layer documented in [VULKAN.md](VULKAN.md). Public contracts live
under [`src/core/include/core`](../../src/core/include/core); implementations
are in [`src/core`](../../src/core).

## Boundaries and build layout

[`src/core/CMakeLists.txt`](../../src/core/CMakeLists.txt) composes these
libraries: header-oriented `common`, `macro`, `meta`, `algorithm`; compiled
`IO`, `net`, `window`, `common` crash handling; `OS`; and `vulkan`. Depend on
the narrowest public header, not a directory-wide umbrella. Platform code is
currently Windows lean-header support in [`OS`](../../src/core/include/core/OS)
and GLFW/Vulkan platform bridges elsewhere.

Core uses assertions for violated programmer contracts. `ASSERT` and
`UNREACHABLE` log a stack trace when logging exists, otherwise write stderr,
then terminate; `HIGH_ASSERT` is compiled out unless enabled. Operational
failure is `bool`/`optional` in I/O and networking, and typed exceptions in
Vulkan.

## Process lifetime, diagnostics, and I/O

- [`AtAppExit`](../../src/core/include/core/common/AtAppExit.hpp) is a
  process-global LIFO callback registry. It is intentionally one-shot; callbacks
  registered by `StaticInitializer` and `Log` must be safe during shutdown.
- [`StaticInitializer`](../../src/core/include/core/common/StaticInitializer.hpp)
  runs `T::init()` exactly once with `std::call_once`, publishes initialization
  with acquire/release atomics, and registers `T::destroy()` at app exit; `Net`
  wraps ENet this way.
- [`Log`](../../src/core/include/core/IO/Log.hpp) owns the global asynchronous
  spdlog logger. `ensureInit` is idempotent, optionally creates `game.log`, and
  arranges teardown; logging macros silently do nothing before initialization.
  Its sink/logger mutation API is not an application-wide synchronization
  protocol—configure it during startup. [`CrashHandler`](../../src/core/common/CrashHandler.cpp)
  installs signal/Windows handlers that log then `quick_exit`.
- [`File`](../../src/core/include/core/IO/File.hpp) reads binary files. `readFile`
  allocates the exact reported size; `readFileTo` requires a destination large
  enough for the entire file and returns `false` for missing/open/read/close
  failures. Both log the failure. Formatting adapters next to `Log.hpp` extend
  `fmt` for core values.

Tests: [`io_tests.cpp`](../../tests/core/io_tests.cpp),
[`log_tests.cpp`](../../tests/core/log_tests.cpp), and
[`static_initializer_tests.cpp`](../../tests/core/static_initializer_tests.cpp).

## Value, byte, range, and control utilities

- [`ByteWriter`](../../src/core/include/core/common/ByteWriter.hpp) writes raw
  object representations of `ByteSerializable` values. Spans/strings are a
  `uint64_t` element count followed by raw bytes. [`ByteReader`](../../src/core/include/core/common/ByteReader.hpp)
  advances only after a complete item and rejects truncated or oversized
  prefixes. Returned span/string views borrow the reader's input bytes; preserve
  that input for their whole use. This is native in-memory serialization, not a
  portable wire format: layout, endianness, and ABI remain part of its contract.
- [`InputSpan`](../../src/core/include/core/common/InputSpan.hpp),
  [`SpanUtils`](../../src/core/include/core/common/SpanUtils.hpp), and
  [`VectorUtils`](../../src/core/include/core/common/VectorUtils.hpp) provide
  zero-copy parameter views and append/byte reinterpretations. `asStringView`
  accepts lvalues and temporary borrowed ranges only; callers retain element
  lifetime and must ensure reinterpretation alignment/size are valid.
- [`EnumBits`](../../src/core/include/core/common/EnumBits.hpp),
  [`TaggedBool`](../../src/core/include/core/meta/TaggedBool.hpp),
  [`Version`](../../src/core/include/core/common/Version.hpp),
  [`TrivialPair`](../../src/core/include/core/common/TrivialPair.hpp), and
  [`HashCombiner`](../../src/core/include/core/common/HashCombine.hpp) are
  value types. `TaggedBool` prevents accidental mixing of independent booleans;
  `EnumBits` requires a dense `Count` enum and a fitting unsigned storage type.
- [`Defer`](../../src/core/include/core/common/Defer.hpp) executes its captured
  lambda on scope exit. `NonCopyable`/`NonMovable` encode ownership; `Assume`,
  `ConstString`, `Color`, and `CodeExecutionHelper` are lightweight contracts/
  aliases. `ControlFlow` stops polling. [`TopoSort`](../../src/core/include/core/algorithm/TopoSort.hpp)
  returns `nullopt` for cycles; successor indices must be in `[0, node_count)`.

Tests: [`byte_io_tests.cpp`](../../tests/core/byte_io_tests.cpp),
[`range_span_tests.cpp`](../../tests/core/range_span_tests.cpp),
[`algorithm_tests.cpp`](../../tests/core/algorithm_tests.cpp), and the focused
`*_tests.cpp` files in [`tests/core`](../../tests/core).

## Compile-time metadata and macros

[`meta`](../../src/core/include/core/meta) provides field traits/predicates,
tagged booleans, enum reflection, enum-to-raw mappings, and compiler-signature
name extraction. `CountableEnum` means `E::Count` exists **and values are dense
from zero**. For a reflected enum, declare `CORE_ENUM_FUNCTIONS(E)` in its
public header and emit exactly one `CORE_ENUM_FUNCTIONS_IMPL(E)` in a source
file; duplicate definitions or missing implementations are linkage failures.
`EnumMapping` anchors permit discontinuous external values; anchors must be
ordered. `FieldRequirement` turns a member pointer into a small predicate.

[`macro`](../../src/core/include/core/macro) centralizes compiler/OS/language
feature detection, attributes, preprocessor counts, and generated identifiers.
It is build-time plumbing; keep feature assumptions local and prefer normal C++
types/contracts in exported APIs.

Tests: [`enum_meta_tests.cpp`](../../tests/core/enum_meta_tests.cpp),
[`field_requirement_tests.cpp`](../../tests/core/field_requirement_tests.cpp),
[`type_name_tests.cpp`](../../tests/core/type_name_tests.cpp), and
[`macro_tests.cpp`](../../tests/core/macro_tests.cpp).

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

## Extension and test checklist

When adding a core subsystem, put public contracts in `include/core/<area>`,
keep lifetime/error semantics explicit, add only needed CMake target edges, and
add a focused suite under `tests/core`. For externally supplied bytes, validate
length and lifetime. For a new enum, decide whether it is dense/reflected before
using `EnumBits` or metadata. For GPU work, continue with [VULKAN.md](VULKAN.md).
