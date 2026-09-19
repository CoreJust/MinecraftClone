# Scenario quickstart

Save a `.core` source with the explicit CoreLang header and
public entry point:

```text
@version("0.1.2")
@use minecraft

fn moveRight(name: str) {
    moveXYZ(name, 1i8, 0i8, 0i8)
}

pub fn scenario() {
    profile("flight3d-v1")
    seed(42u64)
    playerXYZ("alice", '@'c8, 4i32, 4i32, 0i32, 0i16, 0i16, 0i16)
    if true { moveRight("alice") }
    wait(10u64)
    expectXYZ("alice", 9i32, 4i32, 0i32)
}
```

CoreLang values must use the types expected by the host calls: `str`, `c8`,
`i8`, `i16`, `i32`, and `u64`. Functions, `if`, and `for` are ordinary
CoreLang composition; the host calls remain the only scenario effects.

At load time the runner compiles the source, invokes `scenario`, and collects
the typed calls into a private plan. The plan is published to the authoritative
runner only after the whole source succeeds. `wait(10u64)` advances ten fixed
authoritative steps. Inputs submitted before it take effect on the next step;
an expectation observes the current boundary without advancing it.

Run the checked-in examples from the repository root with the scenario command
used by the application. The most useful S5 fixtures are
`s5_flight_boundary.core`, `s5_flight_camera.core`, and
`s5_flight_multiplayer.core`; `s5_flight_invalid.core` must be rejected before
publication. The world fixture is loaded separately from
`scenarios/world/canonical_world.core` and generates seeded chunk content at
world-load time.
