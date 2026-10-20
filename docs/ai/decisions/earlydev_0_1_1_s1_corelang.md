# S1 — CoreLang 0.1.5 dependency contract

The owner requires **CoreLang 0.1.5 with JIT/AOT**, allowing a temporary
development dependency before release. Coordinate with **Resume CoreLang
0.1.5 delivery** (thread `01a14aaf-5a14-7613-a776-b20baa6ae600`). Obtain a
clean ai-dev commit, fetch locator and native/JIT/AOT capability receipt
from that owning chat before treating integration as qualified.

## Temporary pin, fixes and release

Pin the immutable CoreProject2026 SHA in `dependencies.lock.json` and packages.
Record repository, SHA, clean state, compiler/runtime, ABI, backend and build
configuration on every platform; exact provenance remains mandatory. Verify
the declared CoreCpp dependency against the APIs consumed by the game. A
source API or isolated package test does not establish a compatible game build.

Report reproducible CoreLang defects to its owning chat; do not commit changes
to CoreLang from MinecraftClone. Coordinate roughly every twelve hours and
advance to a newer exact commit after focused regression, deterministic-world
parity and affected performance checks. New language features belong to future
versions. Preserve old pins and evidence for rollback.

When 0.1.5 is released, replace the temporary pin with its released immutable
revision/locator and repeat dependency, deterministic-world and platform checks.
Never bypass provenance for final S1 acceptance.

## Backend and module obligations

Keep deterministic CPU terrain kernels shared and CoreLang compilation and host
binding in an adapter. JIT/AOT execute the bounded plan and host calls without
duplicating terrain. Requested backends must be explicit; an unavailable backend
fails rather than silently falling back. Record the actual backend, source and
bytecode identity, compiler, target, and native image identity where applicable.
Compare generation, scenario and policy behavior with the interpreter.

Include world-specific compile/load cost in original-T0 loading measurements.
Assess process/runtime/thread ownership, cancellation boundaries and retained
executable memory. Optimization must preserve Terrain-v2 content for a fixed
version, seed and configuration. Exact package/platform, backend parity and
normal gameplay evidence are separate requirements; coordination alone does
not satisfy them.
