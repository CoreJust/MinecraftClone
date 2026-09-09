# EarlyDev 0.1.0 implementation decisions

This record supports [Initiation](../tasks/MC-AI-0033.md) and snapshots S3–S8.
The product roadmap owns scope and the delivery roadmap owns acceptance. These
are implementation contracts, not release evidence.

## Boundaries and modularity

S3 remains a 32×32 flat, byte-coordinate world with a 100 ms server tick,
same-build codec, and Vulkan grid/player renderer. Shared code owns simulation
and wire contracts; the server owns accepted state; the client owns input and
presentation; core owns GPU resources.

Keep module APIs small and established: expose necessary operations and data
contracts, not storage or implementation details. Keep pure simulation and
geometry separate from window, network, and GPU effects for direct tests.
Prefer compact contiguous world, mesh, and simulation layouts with predictable
iteration and batched work. Do not add allocation, indirection, copying, ECS,
plugin ABI, persistence framework, or transport solely for hypothetical mods;
instead preserve focused low-coupling APIs that make future extension possible
without compromising measured hot paths.

Keep terrain generation deterministic and independent of visual residency.
Future progressive generation may refine distant coarse terrain with structures
and nearby decoration; S6 implements only this minor's air/stone generator,
not those future content stages.

`EarlyDev 0.1.0:3` is declared but not released. S3 decides the version naming
and portable-wire direction; S4–S8 changes remain future commitments. After
Snapshot 8 is published, report its evidence and artifacts here and wait for
the user's hands-on feedback. Do not finalize or publish the minor
automatically. Plan Snapshot 9 only if feedback requires it.

## Coordinates and protocol

From S4, use right-handed X/Y horizontal and Z up. One world unit is one block;
zero yaw faces +Y and +X is right. Camera math performs the Vulkan conversion
once. Simulation positions use named `double` components; block/chunk
coordinates are signed 32-bit; player IDs are uint32. GPU positions are
camera- or chunk-relative floats, never large absolute floats.

By S5, replace native-structure packets with one reliable ENet packet starting
`0x4D`, version `1`, and stable tag (the five current meanings keep tags 0–4).
Encode fields separately in little-endian fixed widths: booleans are 0/1,
input is -1/0/1, identifiers uint32, block coordinates int32, and continuous
coordinates validated IEEE-754 binary64. Reject unknown marker/version/tag,
truncation/trailing bytes, noncanonical booleans, nonfinite/out-of-range data,
and unsupported values before state mutation. Mixed versions fail explicitly;
backward compatibility is not promised. ENet identity selects the controlled
player, never packet content.

## Snapshot architecture

- **S4:** decouple polling/render refresh from the 100 ms authoritative flat
  step using monotonic time and bounded catch-up. Keep camera/controller math
  GLFW-independent; render depth-tested grid and boxes. Rendering cannot cap
  input/network progress, and presentation never mutates authority.
- **S5:** use one compact 16³ air/dirt chunk, cached exposed-face meshes, and
  authoritative flight. Rebuild geometry only for content/neighbour changes;
  the vertex fallback matches optional mesh output.
- **S6:** expose `[0,65536)² × [0,1024)` without dense storage (one byte per
  block would be 4 TiB). Lazily generate bounded resident 16³ chunks with a
  deterministic versioned air/stone function. Eviction/regeneration must be
  identical; visual residency never changes collision authority.
- **S7:** centralize positive X/Y modulo 65536 and shortest wrapped offset for
  simulation, chunk keys, meshing, collision, interest, replication, and
  camera presentation. Z never wraps. Profile a fixed warmed/cold workload
  before selecting optimizations; record p50/p95/max CPU/tick and GPU timing
  when available, residency/mesh bytes, work, and network traffic. Structural
  budgets prohibit work proportional to world volume or travel history.
- **S8:** use a fixed initially 20 ms server step with bounded catch-up and
  collision work. Flight, block phasing, and entity phasing are independent
  server grants. Otherwise apply gravity, grounded jump, solid AABB and entity
  collision, including seams; swept tests/bounded substeps prevent tunnelling.
  Permission revocation must recover a valid position.

## Delivery evidence

Use pure tests for math, codecs, chunks, meshes, wrapping and physics; use
bounded real processes for authority, join/disconnect and replication. Renderer
acceptance must inspect presented content and validation output, not survival.
S3 proves codec rejection, two-client multiplayer, resize/reload/close and
vertex fallback. S4 proves camera/cadence/depth; S5 golden packets and meshing;
S6 boundaries/determinism/cache; S7 seams and profiles; S8 every permission and
collision edge case. Windows/macOS packages need executable-relative shaders
and platform-specific launch evidence. Missing GPU/platform evidence stays
reported as missing, never substituted by unit tests. Every snapshot also ships
an installable Android executable using shared simulation/rendering and narrow
native surface, input, lifecycle and asset adapters. Test the exact released
macOS and Android packages locally; Windows runtime testing belongs to the user.

The user's basic-world target is at least 5,000 FPS on a Ryzen 9 HX laptop with
an RTX 5070 Ti laptop GPU and 32 GB RAM, and several hundred FPS on this Mac.
Weaker laptops must remain smooth. Use 1920×1080 as a provisional benchmark
resolution until specified; record actual framebuffer size, power/build mode,
present mode, hardware and frame-time percentiles. Separate presented frame
rate from CPU submission and offscreen throughput. These targets remain
unverified until measured on the relevant machines; microbenchmarks do not
establish them.
