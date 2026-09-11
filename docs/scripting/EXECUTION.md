# Scenario execution model

## Boundaries, steps, and source order

Setup creates the selected profile’s initial authoritative state at **boundary
0**. It places every declared player and records the initial input as stopped
`(0, 0)`. No authoritative simulation step has occurred at boundary 0.

The command body is interpreted once, from first command to last command. At a
given boundary, commands have the same source-order semantics as ordinary
statements: each command observes all earlier commands at that boundary.

- `input` changes the named actor’s requested direction immediately for the
  command stream, but that direction is consumed only by the **next**
  authoritative step and later steps until another input replaces it.
- `wait N` performs exactly `N` authoritative steps in sequence. A movement
  submitted before the wait is therefore first applied on the first step it
  advances. After it completes, the current boundary has increased by `N`.
- `expect` reads the current boundary and does not advance time.

For the canonical sample, `input alice 1 0` is set at boundary 0; `wait 5`
advances steps 1 through 5; Alice reaches `(9, 4)` at boundary 5. The
subsequent stopped input is at boundary 5 and cannot retroactively affect
those five steps.

The scenario runner is authoritative. An expectation is evaluated against the
authoritative profile state, never client prediction, renderer state, wall
clock time, or transport delivery order. A failed expectation reports a
diagnostic and stops successful scenario completion.

For `flat3d-v1`, `input ACTOR camera STRAFE FORWARD` is resolved from that
actor's recorded yaw before it is sent through the unchanged `Direction` wire
message. Yaw zero maps forward to +Y and positive yaw turns forward toward +X.
Pitch and roll are replay metadata only. The authoritative server's 100 ms
fixed-tick delay is calculated from each tick's elapsed server work; it has no
renderer or presentation input. Scenario tick barriers directly invoke that
same authoritative tick and are not measurements of display refresh.

## Validation, limits, and safety

The runner must parse and fully validate the whole source before changing
world state. Validation includes grammar, profile, all references, profile
ranges, duplicate declarations, arithmetic conversion, and every configured
limit. A rejected source has no setup, input, step, network, filesystem, or
other execution side effect.

The host supplies immutable resource limits. Scripts cannot declare, override,
or raise them. At minimum, hosts bound:

| Limit | What it bounds |
| --- | --- |
| Source bytes | Entire input source, before parsing. |
| Statements | Every nonblank directive line accepted in one source, including the header, setup, structural markers, and body commands. |
| Actors | Declared players. |
| Ticks | Total authoritative steps requested by all `wait` commands. |
| Operations | Emitted `input`, `wait`, and `expect` plan operations. |
| Evidence | `expect` operations recorded as plan evidence. |

Host limit configuration is part of the runner’s trusted configuration. Every
limit must be positive; an invalid configuration is rejected before a script
can run. It is not silently clamped, ignored, or repaired from script input.
Limits are checked with overflow-safe arithmetic.

## Authority and safety

Scenario source is the current finite scenario-automation subset. It requests
only the typed, host-controlled authoritative operations defined below; it is
not a general-purpose extension or operating-system automation environment. It
has no loops, expressions, variables, imports, arbitrary file access, process
or shell execution, direct network access, wall-clock access, random source
other than the declared profile seed, or host-configuration API. It cannot
spawn arbitrary processes, change the board beyond its declared bounded
players, contact a server directly, or raise resource limits beyond the fixed
grammar and selected profile contract.

The only effects a valid script can request are the profile-defined initial
state, player inputs, bounded authoritative steps, and bounded state checks.
The embedding host remains responsible for deciding whether it records
evidence, exposes results, or connects its simulation to external services.

## Versions and profiles

`scenario 1` selects this grammar; `flat2d-v1` and `flat3d-v1` select their
explicit world/replay rules. Both
are explicit compatibility boundaries. A runner must reject an unsupported
format version or profile rather than guessing, falling back, or silently
changing semantics.

Any future format or profile must use a new, explicit version/profile
identifier. Existing `scenario 1` and `flat2d-v1` sources retain their
documented interpretation. In particular, a future runner must not silently
reinterpret a `wait` as a different tick rate or as elapsed wall time. Any
tick-rate policy change needs a new explicit compatibility contract.

A future compiler, VM, or JIT could consume the same typed host operations, but
none is implemented or part of current behavior. The public contract is the
finite source grammar and authoritative semantics above.
