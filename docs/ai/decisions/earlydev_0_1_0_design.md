# EarlyDev 0.1.0 implementation decisions

Implementation contracts for [Initiation](../tasks/MC-AI-0033.md), S3–S8;
the roadmaps own scope and acceptance. This is not release evidence.

## Boundaries and modularity

S3 remains a 32×32 flat, byte-coordinate world with a 100 ms server tick,
same-build codec, and Vulkan grid/player renderer. Shared code owns simulation
and wire contracts; the server owns accepted state; the client owns input and
presentation; core owns GPU resources.

Use small encapsulated APIs. Separate pure simulation and geometry from window,
network and GPU effects. Prefer contiguous data, predictable iteration and
batching. Preserve source-level extension points without speculative ECS,
binary plugin ABI, persistence/transport frameworks or unmeasured hot-path
allocation, indirection and copying.

Generation stays deterministic and independent of visual residency. S6 supplies
the required registered region/chunk stages, explicit bounded retry and simple
air/stone proof; rich content stays future. [Package architecture](architecture_packages.md)
owns the module, scripting, kernel, audio and LOD contracts.

`EarlyDev 0.1.0:3` is declared but not released. S3 decides the version naming
and portable-wire direction; S4–S8 changes remain future commitments. After
Snapshot 8 is published, report its evidence and artifacts here and wait for
the user's hands-on feedback. Do not finalize or publish the minor
automatically. Plan Snapshot 9 only if feedback requires it.

## Coordinates and protocol

From S4, use right-handed horizontal X/Y and Z up. One unit is one block; zero
yaw faces +Y and +X is right. Convert for Vulkan once. Simulation uses named
`double` components, signed 32-bit block/chunk coordinates and uint32 player
IDs. GPU positions are camera- or chunk-relative floats, never large absolutes.

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

## Configuration and scripting

S3 introduces a versioned finite text language that
parses into a validated immutable scenario plan before changing state. Execute
setup, player input, fixed tick delays and assertions through authoritative
simulation operations, never in the render loop. Profiles preserve coordinate
and tick semantics; unsupported profiles fail rather than reinterpret old
scripts. Later snapshots add camera, block, world-generation, wrapping and
permission operations with their features. The same runner powers playthroughs and executable
acceptance. S4 keeps `scenario 1` and `flat2d-v1` fully supported while new
documentation may prefer the generic Core language and Shared DSL. Keep
host-enforced resource limits and precise source diagnostics;
remote player packets never acquire script/setup authority. A future compiler,
VM or JIT can target these operations but is not part of this minor. Provide a
navigable language guide, complete grammar/reference and tested examples.

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

Targets: 5,000 full-frame render/submit/present-request game-loop iterations per
second on the Ryzen 9 HX/RTX 5070 Ti/32 GB Windows laptop, several hundred on
this Mac, and smooth weaker-laptop operation. This does not mean 5,000 physical
screen refreshes; measure displayed cadence separately. At provisional
1920×1080, record framebuffer, power/build mode, present mode, hardware and
percentiles. Targets remain unverified; renderer-only/offscreen/CPU
microbenchmarks do not establish them.
