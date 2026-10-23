# Entity permissions

[Policy.hpp](../../src/shared/include/shared/policy/Policy.hpp) defines
server-owned policies for player, mob, and technical-entity subjects. CoreLang
declarations create the built-in `all`, `players`, `mobs`, `entities`, and
per-kind groups, plus bounded explicit and selector groups. The `entities`
group means technical entities, not all entity kinds. A selector matches one
entity kind within an inclusive entity-ID range and resolves its membership
before publication. Explicit groups list current entity IDs; individual
overrides use `entity:<id>` targets. The reserved `world` and `world:` namespace
is rejected.

Capability keys are namespaced and define defaults, value bounds, and a hard
restriction direction. Soft rules resolve by target specificity; equal-
specificity ties choose the more restrictive value for that capability key.
Hard rules combine to the strongest constraint and only tighten the result. The compiler bounds source
bytes, VM instructions, statements, groups, members, selectors, presets, rules,
assignments, subjects, and capabilities. Its static instruction bound covers
module initialization, policy entry, reachable helpers, and requested
interpreter-parity runs. Finite `for` loops are accepted only with matching,
nonnegative fixed-width integer literal endpoints and an immutable named index;
index writes, aliases, captures, and address escapes are rejected. Dynamic
loops, source imports, indirect calls, recursion, and bytecode static-init
instructions are rejected. This is a preflight bound, not runtime instruction
fuel. Cancellation is polled during bounded analysis and compilation, around
backend phases, and at host-call boundaries; a running runtime or native call
does not receive instruction-by-instruction cancellation.

`PolicyHost` compiles and materializes outside physics ticks. Materialization
checks the subject and key catalogs, resolves selectors, and creates immutable
dense capability rows bound to their exact plan. Publication assigns a
generation. Snapshot replacement uses release publication and acquire reads;
libraries with atomic shared-pointer support use that specialization, while
older libc++ retains its supported atomic shared-pointer functions.
Indexed queries use a two-argument subject/key query and fail closed for
unknown subjects or keys. Both query values are required at construction, so an
incomplete query cannot silently default to entity or capability index zero. The
query takes distinct, non-convertible subject-ID and capability-key-ID types, so
their arguments cannot be accidentally reversed; the snapshot still stores the
same integer IDs and performs the same dense lookup.
The server validates movement values and collision geometry before publishing,
then applies and replicates changed player capabilities. Policy behavior is
covered by `tests/core/corelang_policy_tests.cpp` and server integration tests.
Disconnect publication removes a departed ID from explicit groups and
entity-targeted assignments, so a later reused ID cannot inherit those concrete
memberships or overrides.
