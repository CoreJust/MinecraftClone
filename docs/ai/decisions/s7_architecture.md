# S7 architecture: authority, policy and presentation

This decision refines [Snapshot 7](../tasks/MC-AI-0038.md) on top of the
verified S6 baseline. CoreLang 0.1.3.2 source and public API are finalized
design input. Only its released, provenance-verified package may enter the
game dependency graph.

## Authority boundary

The server owns entity identity, kind, authoritative transform, collision
shape, palette identity, permissions, world revision and materialization. A
client sends movement intent and renders replicated presentation data; it
cannot elevate permissions, select a collision result or publish world state.

An entity subject has a stable identity and kind. S7's built-in groups are
`all`, `players`, `mobs`, `entities` for technical non-player/non-mob
entities, and one group for every entity kind. Custom groups contain explicit
members or bounded selector results. The subject namespace reserves a distinct
future world-subject kind without implementing world permissions in S7.

## Permission compilation seam

Policy mutation follows this boundary:

```text
validated policy and membership changes
  -> policy compiler backend
  -> immutable CapabilitySnapshot { generation, dense capability bits,
                                    bounded parameter tables }
  -> authoritative fixed-tick physics
```

Hard policy is the inherited maximum permission envelope: descendants and
individuals may only narrow it. Soft policy is an overridable default. The
resolver freezes deterministic precedence and denial wins unresolved
authoritative conflicts. Group traversal, selector evaluation, string-key
resolution, CoreLang execution, compilation and cache publication occur only
at mutation time. Physics reads a generation-tagged snapshot through constant
time simple checks or bounded parameter lookups; unknown subjects and keys
fail closed.

CoreLang may supply authored declarations, selector/preset logic and selected
measured kernels behind the policy compiler backend. The host owns validation,
resource limits, scheduling, snapshot publication and failure semantics.
Preparation is outside ticks, required preparation is atomic, and each chosen
kernel has interpreter parity. Rendering is not a consumer of this backend.

## Presentation seam

The first CoreLang-free slice keeps `CameraMode` local to a client and resolves
it from the local replicated entity pose, look angles, projection dimensions
and a bounded obstruction-query interface. It returns a camera pose and local
body visibility only. First-person hides the local parallelepiped; rear and
front-facing third-person show it. Cycling, clipping and visibility never
change transmitted intent, authority, permissions or collision shape.

Each player receives a server-owned deterministic four-bit palette index from
the documented 16-color palette. It replicates with player state and survives
reconnect. The renderer consumes only the resulting presentation snapshot.

## S7 task order

- MC-AI-0243 records this architecture and release gates.
- MC-AI-0246 implements the CoreLang-free palette/body/camera slice now.
- MC-AI-0244 waits for the exact CoreLang 0.1.3.2 package; MC-AI-0245 then
  implements entity-wide permissions and the physics consumer.
- MC-AI-0247 adds deterministic impaired-network acceptance; MC-AI-0248
  measures the complete candidate, including the `x200` speed modifier.
- MC-AI-0249 makes sanitizer and static-analysis receipts a fail-closed S7
  release requirement. The later CoreCpp work follows the same matrix.
