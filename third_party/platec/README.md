# PyPlatec native source closure

This directory contains the native C++ implementation and verbatim LGPL
license from the frozen PyPlatec 1.4.1 single-Y source variant. The source
commit, original and variant file hashes, compiled translation units, and
excluded wrappers are recorded in [provenance.json](provenance.json).

The native variant makes two source changes for the periodic single-Y world:
`noise.cpp` and `simplexnoise.cpp` use a full `2*pi` angular wrap on Y. Their
hashes are bound to the frozen variant in the provenance record. The build
omits `platecapi.cpp` and `platecapi.hpp`, which own an unsynchronized global
registry, and the Python extension wrapper. `mc_generation` owns each
`lithosphere` instance directly.

## Private v9 stage contract

[`V9PlatecSource.cpp`](../../src/generation/V9PlatecSource.cpp) maps the full
unsigned 64-bit user seed twice. Each SHA-256 message is ASCII
`namespace NUL decimal-seed NUL domain NUL 0`, where the final `0` is the
decimal counter. The topology domain is
`world7-pyplatec-topology-mixture-candidate-v1` / `topology-regime`; its first
eight digest bytes form a big-endian selection value. The native engine domain
is `world7-pyplatec-default-v1` / `native-engine-seed32`; its first four bytes
form the big-endian 32-bit Platec seed. These are independent domains; the
stage does not use the game's NumPy PCG64 stream.

The source run is 256 by 256 with one restart cycle, erosion period 60, folding
ratio 0.02, and aggregation overlaps 1,000,000 / 0.33. The frozen selection
cuts divide topology into 10% supercontinent-oriented (6 plates, 0.75 initial
ocean fraction), 20% mixed-landmass (24, 0.75), and 70% fragmented-landforms
(24, 0.90). Outputs retain Platec raw `float` height and current `uint32` plate
ownership; they are not gameplay units or ancestry.

Progress reports the initial state and each native update, bounded at 2,400
updates. Cancellation is checked before construction and between updates and
returns no partial result. Platec's terminal restart deletes plates but leaves
the ownership map. The result therefore tags its real, finite plate direction
vectors as `ActiveBeforeTerminalRestart`, with the native step after which that
population was observed. They are not directions from the terminal state.

The [eight seed vectors](../../tests/fixtures/terrain_v9/seed_bridge.tsv)
include zero, boundary values and `UINT64_MAX`. The [raw source oracles](../../tests/fixtures/terrain_v9/source_oracles.tsv)
bind height, ownership and final-step digests for seeds 10, 2 and 42. AppleClang
17 Release matches all three and passes the focused 8-test stage suite. MSVC
19.44's standalone native-module build matches all three and passes 7/7 stage
tests. Its signed-wrap edge test also passes 1/1 on the exact digit-separated
source. Windows uses `/fp:precise` for Platec, `/fp:strict` for the wrapper and
SHA code, and no `/fp:contract` or `/arch:AVX2`.

The v9 execution path spells the frozen contracted operations with explicit
`std::fma` calls and disables implicit contraction for Platec. The seed-square
remainder preserves the source's two's-complement wrap and signed `% 256` using
defined unsigned multiplication and `bit_cast`. This isolates the compatibility
behavior from CPU feature flags and R6's strict-FP setting. The standalone
Windows result establishes raw-source parity; it does not establish full-game
runtime or performance acceptance. Other compiler/platform combinations remain
unverified.

The upstream LGPL-3.0-or-later license is retained as `LICENSE`. The frozen
NoiseLib dependency closure contains no separate license file. Resolve its
notice and license status before product redistribution; this record does not
make a legal determination.
