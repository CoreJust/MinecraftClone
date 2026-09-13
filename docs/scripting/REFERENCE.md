# Scenario language reference

This is the complete grammar for source format version `1`. Each declaration,
structural marker, and command occupies one logical line. Spaces and tabs
separate tokens within that line; a line ending terminates it.

## Lexical rules

- An identifier is ASCII `[A-Za-z_][A-Za-z0-9_-]*`.
- An integer is base-10 only: `-?[0-9]+`. Hexadecimal, binary, decimal-point,
  exponent, `+`-prefixed, and non-finite forms are invalid. Fields that name a
  version, seed, count, or coordinate further require a non-negative value.
- A character is a quoted string containing exactly one supported character.
  The current profile has no string escape syntax.
- `#` starts a comment outside a quoted string, including after a command.
  It is not a comment marker inside a character string, so `character "#"` is
  valid.
- Blank lines are ignored. LF, CRLF, and CR line endings are accepted.

## Grammar

The notation uses `*` for zero or more repetitions, `+` for one or more, `|`
for alternatives, and quoted text for literal tokens. Commas between the
top-level productions require a logical line boundary; tokens inside a
production are separated by spaces or tabs.

```ebnf
file          = scenario_header, profile, seed, player_declaration+, "begin",
                command*, "end", EOF ;
scenario_header = "scenario", format_version ;
format_version = nonnegative_integer ;
profile       = "profile", identifier ;
seed          = "seed", nonnegative_integer ;
player_declaration = player_2d | player_3d ;
player_2d     = "player", identifier, "character", character,
                "at", coordinate, coordinate ;
player_3d     = "player", identifier, "character", character,
                "at", coordinate, coordinate, coordinate, "orientation",
                yaw_degrees, pitch_degrees, roll_degrees ;
command       = input | wait | expect_position ;
input         = direction_input | camera_input ;
direction_input = "input", identifier, direction, direction ;
camera_input  = "input", identifier, "camera", direction, direction ;
wait          = "wait", positive_integer ;
expect_position = expect_position_2d | expect_position_3d ;
expect_position_2d = "expect", "player", identifier, "position",
                     coordinate, coordinate ;
expect_position_3d = "expect", "player", identifier, "position",
                     coordinate, coordinate, coordinate ;
identifier    = ASCII letter or "_", { ASCII letter | digit | "_" | "-" } ;
integer       = [ "-" ], digit, { digit } ;
nonnegative_integer = digit, { digit } ;
positive_integer = digit_nonzero, { digit } ;
coordinate    = integer ;
direction     = integer ;
character     = '"', one profile-supported character, '"' ;
EOF           = end of source ;
```

The order is strict: header, profile, seed, at least one player, `begin`, zero
or more commands, `end`, then EOF. A header or command cannot be moved,
duplicated, omitted, or followed by another declaration after `begin`.

## Commands

| Command | Meaning | Validation |
| --- | --- | --- |
| `input ACTOR DX DY` | Submit the named player’s direction for subsequent authoritative steps. | `ACTOR` was declared; each component is `-1`, `0`, or `1`. |
| `wait N` | Advance exactly `N` authoritative simulation steps. | `N` is a positive base-10 integer and does not exceed remaining script/host budgets. |
| `expect player ACTOR position X Y` | Compare the named player’s authoritative position at the current boundary. | `ACTOR` was declared; `X` and `Y` are valid profile coordinates. |

## `flat2d-v1` profile

`flat2d-v1` provides:

| Item | Contract |
| --- | --- |
| Board | Coordinates `(x, y)` with each component in `0..31`. |
| Characters | Exactly one of `@`, `#`, `$`, `%`, `&` for each player declaration. |
| Players | At least one declaration; identifiers and characters are unique. Initial cells must differ by at least two cells on one axis, keeping each player outside every other's 3 by 3 exclusion zone. |
| Directions | Each `DX` and `DY` is an integer in `-1..1`. |
| Waiting | `N` is an integer greater than zero. |
| Seed | Recorded deterministic configuration. This profile currently performs no random operation, so changing the seed has no current state effect. |

Profile validation happens before any setup or simulation mutation. The
[execution model](EXECUTION.md) defines how a profile evolves in future source
versions.

## `flat3d-v1` profile

The legacy `scenario 1` frontend also supports `flat3d-v1`. It is a replay and
control profile over the current authoritative flat board, not vertical-world
simulation. It uses `player_3d`, `expect_position_3d`, and `camera_input`.

| Item | Contract |
| --- | --- |
| Coordinates | `x` and `y` are board coordinates in `0..31`; `z` is explicitly recorded but must be `0`. |
| Orientation | `yaw` is `0..359`, `pitch` is `-89..89`, and `roll` is `-180..180`, all in degrees. |
| Camera input | `input ACTOR camera STRAFE FORWARD` accepts `-1..1` components. Yaw `0` faces +Y and positive yaw turns toward +X. The runner resolves it to the unchanged cardinal authoritative `Direction`; a diagonal tie chooses X. |
| Evidence | Runtime records a deterministic plan replay ID, camera-relative input count, and the 100 ms authoritative tick. Wall-clock elapsed time is not part of the replay ID. |

The existing CoreLang frontend remains limited to `flat2d-v1`; it continues to
lower unchanged 2D source into the same authoritative plan.
